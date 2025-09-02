#include "PlayMode.hpp"
#include "Load.hpp"
#include "load_save_png.hpp"

//for the GL_ERRORS() macro:
#include "gl_errors.hpp"

//for glm::value_ptr() :
#include <glm/gtc/type_ptr.hpp>

#include <random>
#include <iostream>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <queue>

#include <algorithm>
#include <cmath>
#include <corecrt_math_defines.h>

/*************************************
 * Game Logic Objects
 *************************************/
struct GameObject {
    // data is stored as floats, but rendered on screen with uint64_t

	// positional info
	std::array<float, 2> position; // center
	std::array<uint8_t, 2> width_radius; // left, right
	std::array<uint8_t, 2> height_radius; // up, down
	std::array<uint8_t, 2> sprite_center; // "software" sprite, bl corner is (0, 0)
};

struct PhysicsObject {
    // physics info
	std::array<float, 2> velocity;
    const std::array<float, 2> gravity; // pixels per second^2
};

struct Player {
    // positional info
    GameObject gameObject = {gameObject.position = {127, 73},
							 gameObject.width_radius = {4, 5},
							 gameObject.height_radius = {8, 9},
							 gameObject.sprite_center = {3, 7}};

    // physics info
    PhysicsObject physicsObject = {{0, 0}, {0, -320}};
	const float RUN_SPEED = 40; // pixels per second
	const float INIT_JUMP_SPEED = 160; // pixels per second
	const float RUN_ACCEL = 200; // pixels per second^2, while left/right is pressed
	const float RUN_DECEL = 200; // pixels per second^2, while left/right are not pressed
	
	// jump info
	bool airborne = false;

	// gemstar info
	bool gemstar_available = false;
	float gemstar_timer = 0;
	const float GEMSTAR_COOLDOWN = 2;

	void update(float elapsed) {
		// if (left.pressed) player_at.x -= PlayerSpeed * elapsed;
		// if (right.pressed) player_at.x += PlayerSpeed * elapsed;
		// if (down.pressed) player_at.y -= PlayerSpeed * elapsed;
		// if (up.pressed) player_at.y += PlayerSpeed * elapsed;
	}
};

struct Gemstar {
    GameObject gameObject = {gameObject.position = {0, 0},
							 gameObject.width_radius = {6, 3},
							 gameObject.height_radius = {6, 3},
							 gameObject.sprite_center = {5, 5}};
    PhysicsObject physicsObject = {{0, 0}, {0, 0}};
	const float BASE_SPEED = 512;

    bool active = false;
};

struct Enemy {
    // positional info
    GameObject gameObject = {gameObject.position = {0, 0},
							 gameObject.width_radius = {4, 5},
							 gameObject.height_radius = {4, 5},
							 gameObject.sprite_center = {3, 3}};

    // physics info
    PhysicsObject physicsObject = {{0, 0}, {0, 0}};

    // behavioral information
};

struct Bullet {
    GameObject gameObject = {gameObject.position = {0, 0},
							 gameObject.width_radius = {3, 3},
							 gameObject.height_radius = {3, 3},
							 gameObject.sprite_center = {2, 2}};
    PhysicsObject physicsObject;

    bool active = false;
};

struct Gem {
    GameObject gameObject = {gameObject.position = {0, 0},
							 gameObject.width_radius = {8, 9},
							 gameObject.height_radius = {8, 9},
							 gameObject.sprite_center = {7, 7}};
    uint8_t shape; // 0-3
};


typedef std::array< std::array< glm::u8vec4 , 8 >, 8 > ColoredTile;
typedef std::unordered_map< glm::u8vec4, uint8_t > PaletteBucket;

std::array<PPU466::Sprite, 4> playerSprites; // includes gemstar and cursor
											 // (array since it will be fixed size)
std::array<PPU466::Sprite, 16> gemSprites; // there will be at most 4 gems in play
										   // which are 4 hardware sprites each

std::vector<PPU466::Sprite> enemySprites; // vector since this can be variable (though bounded by 12)
std::vector<PPU466::Sprite> bulletSprites; // vector since this can be variable

PPU466 ppu = PPU466();

/*************
 * Game Logic
 *************/
const uint8_t MAX_ENEMIES = 12;
const uint8_t MAX_BULLETS = 32;
const glm::u16vec2 UPPER_CORNER = {256, 240};
const uint64_t GROUND_LEVEL = 64;

/*****************
 * Sprite Indices
 *****************/
const uint8_t FIRST_PLAYER_SPRITE = 0;
const uint8_t FIRST_GEM_SPRITE = 4;
const uint8_t FIRST_ENEMY_SPRITE = 20;
const uint8_t FIRST_BULLET_SPRITE = 32;
uint64_t flicker_idx = 0;

Player player;
Gemstar gemstar;
std::vector<Enemy> enemies;
std::vector<Bullet> bullets;

int color_compare(glm::uvec4 color_a, glm::uvec4 color_b) {
	for (int i = 0; i < 4; i++) {
		if (color_a[i] < color_b[i])
			return -1;
		else if (color_a[i] > color_b[i])
			return 1;
	}
	return 0;
}
bool palette_match(std::vector<glm::u8vec4> colors, std::vector<glm::u8vec4> cmpPalette) {
	if (colors.size() > cmpPalette.size()) {
		// check if colors is a strict superset
		for (uint8_t p = 0; p < cmpPalette.size(); p++) {
			bool color_found = false;
			for (uint8_t c = 0; c < colors.size(); c++) {
				if (colors[c] == cmpPalette[p])
					color_found = true;
			}
			if (!color_found)
				return false;
		}
		return true;
	}
	else {
		// check if colors is a strict subset of cmpPalette
		// or if colors and cmpPalette match
		for (uint8_t c = 0; c < colors.size(); c++) {
			bool color_found = false;
			for (uint8_t p = 0; p < cmpPalette.size(); p++) {
				if (colors[c] == cmpPalette[p])
					color_found = true;
			}
			if (!color_found)
				return false;
		}
		return true;
	}
}

/*****************************
 * Asset Pipeline Functions
 *****************************/
void process_tiles(PPU466* ppu) {
	/***********************************************************************************
	 * TILES
	 * Based on code provided by Jim McCann.
	 * 
	 * 1) Scan and get spritesheet data into an array
	 * 2) Do index math to get "png tiles" (8x8 blocks)
	 * 3) Calculate Palette Indices from colors:
	 *    a) put tiles into buckets based on colors found
	 *    b) if a tile has no conflicting colors with a bucket,
	 *       it goes in that bucket (bucket gets updated). Create buckets as needed
	 *    c) Go through each bucket and assign each color a number 0-3
	 * 		 (transparent (0xeeeeee) is 0, 1-3 are sorted smallest hex value to largest)
	 *    d) Create tiles based on number
	 *    e) (if time) create flipped versions of tiles?
	************************************************/

	//1) Scan for data
	const uint64_t spritesheet_dimensions = 64*48;
	const uint64_t tile_count = 46;
	const uint64_t true_pixel_count = (64*48);
	glm::uvec2 *spritesheet_size = new glm::uvec2(64, 48);
	std::vector< glm::u8vec4 > *raw_tile_pixels;

	// ColoredTile holds color info rather than palette index info (we'll use it to construct the palette info for the tile)
	std::array< ColoredTile, tile_count > colored_tiles;
	load_save_png:load_png("assets/palettes/spritesheet.png", spritesheet_size, raw_tile_pixels, OriginLocation::UpperLeftOrigin);

	// 2) Index math
	for (uint64_t r = 0; r < true_pixel_count / 8; r++) {
		// 8-pixel row by 8-pixel row, assign to the right tile
		std::array< glm::u8vec4, 8 > pixel_row;
		
		// go across the pixels and get the row
		for (int p = 0; p < 8; p++) {
			pixel_row[p] = (*raw_tile_pixels)[(8 * r) + p];
		}

		// then, assign the pixel row to the right tile:
		// get the row and the column it belongs in on the tbale
		// then calculate the index in the table tile
		uint64_t table_row = r / 64;
		uint64_t table_col = r % 8;
		uint64_t table_idx = (table_row * 8) + table_col;

		if (table_idx < tile_count) {
			uint64_t row_num = (r / 8) % 8; // row in the tile
			colored_tiles[table_idx][row_num] = pixel_row;
		}
	}

	// 3) Palette indices
	// mapping of rgba -> number is a bucket
	// technically an ordering, but easier to use unordered_map
	std::vector<PaletteBucket> palette_buckets = {};
	std::unordered_map<ColoredTile, PaletteBucket*> tile_palette_map = {}; // used to assign palette indices
	for (uint64_t t = 0; t < tile_count; t++) {
		ColoredTile tile = colored_tiles[t];

		// a) get the colors of the tile
		std::vector<glm::u8vec4> colors; 
		for (auto row : tile) {
			for (uint8_t p = 0; p < 8; p++)
				colors.emplace_back(tile[p]);
		}

		// b) determine which PaletteBucket it goes in, and add to the tile_palette_map
		//	  (update buckets as necessary)

		// to compare palettes against the current
		std::vector<PaletteBucket> palettes;
		for (auto [tile, bucket] : tile_palette_map) {
			palettes.emplace_back(bucket);
		}
		
		// compare `colors` to each palette
		for (uint64_t p = 0; p < palettes.size(); p++) {
			PaletteBucket bucket = palettes[p];
			std::vector<glm::u8vec4> cmpPalette = {};
			for (auto [color, num] : bucket)
				cmpPalette.emplace_back(color);

			if (palette_match(colors, cmpPalette)) // first palette to match, map the 
			{
				// the larger palette is what the tiles are mapped to
				if (cmpPalette.size() < colors.size())
				{
					// update the bucket with the colors in `colors`
					for (auto color : colors)
						bucket.emplace(color);
				}
				tile_palette_map.emplace(tile, bucket);
				break;
			}
		}
	}

	for (PaletteBucket bucket : palette_buckets) {
		// c) Go through each bucket and reassign numbers
		// get keys
		std::vector<glm::u8vec4> keys;
		for (auto [color, num] : bucket) {
			keys.emplace_back(color);
		}

		// sort and remap
		for (uint8_t i = 0; i < 4; i++) {
			for (uint8_t j = i; j < 4; j++) {
				if (color_compare(keys[j], keys[i]) < 0 || (keys[j][0] == 0xee && keys[j][1] == 0xee && keys[j][2] == 0xee)) {
					glm::uvec4 temp = keys[i];
					keys[i] = keys[j];
					keys[j] = temp;
				}
			}
			bucket[keys[i]] = i;
		}
	}

	for (uint64_t t = 0; t < tile_count; t++) {
		// static_assert(tile_palette_map.contains(tile_colors[t]), "Colored tile %lu was added to the tp map");
		// d) construct tile
		ColoredTile tile = colored_tiles[t];
		PaletteBucket palette_bucket = *(tile_palette_map[tile]); // color->int

		for (uint64_t y = 0; y < 8; y++) {
			std::array< glm::u8vec4, 8 > tile_row = tile[y];
			uint8_t row_bit_0 = 0;
			uint8_t row_bit_1 = 0;
			for (uint64_t x = 0; x < 8; x++) {
				glm::u8vec4 color = tile_row[x];
				uint8_t palette_idx = palette_bucket[color];
				// (x,y) -> color -> palette_idx (from PaletteBucket)
				row_bit_0 += (palette_idx % 2) << (7 - x);
				row_bit_1 += (palette_idx / 2) << (7 - x);
			}
			(*ppu).tile_table[t].bit0[y] = row_bit_0;
			(*ppu).tile_table[t].bit1[y] = row_bit_1;
		}
	}
}
void process_palettes(PPU466* ppu) {
	/*********************************************************************************
	 * PALETTES
	 * Palettes are also pngs, use load_png to convert to color format.
	 * Assumes palette is already sorted
	 * (with the exception of "eeeeee" leading if present,
	 *  as that marks transparency)
	 *********************************************************************************/
	glm::uvec2 *palette_sheet_size = new glm::uvec2(4, 8);
	std::vector< glm::u8vec4 > *palette_data;
	load_save_png:load_png("assets/palettes/palettes.png", palette_sheet_size, palette_data, OriginLocation::UpperLeftOrigin);
	for (uint64_t i = 0; i < (*palette_data).size(); i++) {
		uint64_t pal_tbl_idx = i / 4;
		uint64_t pal_idx = i % 4;

		if (pal_idx == 0 && (*palette_data)[i][0] == 0xee && (*palette_data)[i][1] == 0xee && (*palette_data)[i][2] == 0xee)
			(*ppu).palette_table[pal_tbl_idx][0] = {0x00, 0x00, 0x00, 0x00};
		else
			(*ppu).palette_table[pal_tbl_idx][pal_idx] = (*palette_data)[i];
	}
}
void create_player_sprites(std::array<PPU466::Sprite, 4> playerSprites) {
	for (int i = 0; i < 4; i++)
		playerSprites[0].index = 0; // head, body, gemstar, reticle

	playerSprites[0].attributes = 0x00;
	playerSprites[1].attributes = 0x00;
	playerSprites[2].attributes = 0x01;
	playerSprites[3].attributes = 0x02;
}

PlayMode::PlayMode() {
	//Asset Pipeline
	process_tiles(&ppu);
	process_palettes(&ppu);

	/**********************************
	 * Sprite (entity) lists
	 **********************************/
	create_player_sprites(playerSprites);
	for (int i = 0; i < 4; i++)
		ppu.sprites[i] = playerSprites[i];
	
}

PlayMode::~PlayMode() {
}

bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &window_size) {

	if (evt.type == SDL_EVENT_KEY_DOWN) {
		if (evt.key.key == SDLK_LEFT) {
			left.downs += 1;
			left.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_RIGHT) {
			right.downs += 1;
			right.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_UP) {
			up.downs += 1;
			up.pressed = true;
			return true;
		} else if (evt.key.key == SDLK_DOWN) {
			down.downs += 1;
			down.pressed = true;
			return true;
		}
	} else if (evt.type == SDL_EVENT_KEY_UP) {
		if (evt.key.key == SDLK_LEFT) {
			left.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_RIGHT) {
			right.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_UP) {
			up.pressed = false;
			return true;
		} else if (evt.key.key == SDLK_DOWN) {
			down.pressed = false;
			return true;
		}
	}

	return false;
}

void PlayMode::update(float elapsed) {

	constexpr float PlayerSpeed = 30.0f;
	// if (left.pressed) player_at.x -= PlayerSpeed * elapsed;
	// if (right.pressed) player_at.x += PlayerSpeed * elapsed;
	// if (down.pressed) player_at.y -= PlayerSpeed * elapsed;
	// if (up.pressed) player_at.y += PlayerSpeed * elapsed;

	if (left.pressed) {
		player.physicsObject.velocity[0] -= player.RUN_ACCEL;
		player.physicsObject.velocity[0] = std::min(player.physicsObject.velocity[0], -player.RUN_SPEED);
	}
	if (right.pressed) {
		player.physicsObject.velocity[0] += player.RUN_ACCEL;
		player.physicsObject.velocity[0] = std::max(player.physicsObject.velocity[0], player.RUN_SPEED);
	}
	if (up.pressed && !player.airborne) {
		player.physicsObject.velocity[1] += player.INIT_JUMP_SPEED;
	}

	player.physicsObject.velocity[0] += player.physicsObject.gravity[0];
	player.physicsObject.velocity[1] += player.physicsObject.gravity[1];

	if (player.gameObject.position[1] < GROUND_LEVEL + player.gameObject.height_radius[1]) {
		player.airborne = false;
		player.gameObject.position[1] = GROUND_LEVEL + player.gameObject.height_radius[1];
	}

	//reset button press counters:
	left.downs = 0;
	right.downs = 0;
	up.downs = 0;
	down.downs = 0;
}

void PlayMode::draw(glm::uvec2 const &drawable_size) {
	//--- set ppu state based on game state ---

	uint32_t bg_size = PPU466::BackgroundWidth * PPU466::BackgroundHeight;
	for (uint32_t t = 0; t < bg_size - 256; t++) {
		ppu.background[t] = (1 << 8) + 28; //0x011C;
	}
	for (uint32_t t = bg_size - 256; t < bg_size; t++) {
		ppu.background[t] = (1 << 8) + 29; //0x011D;
	}

	playerSprites[0].x = player.gameObject.position[0] - player.gameObject.width_radius[0];
	playerSprites[0].y = player.gameObject.position[1] + 1;
	playerSprites[1].x = player.gameObject.position[0] - player.gameObject.width_radius[0];
	playerSprites[1].y = player.gameObject.position[1] - player.gameObject.height_radius[1];

	/******************************************************************
	 * TODO: Things to draw:
	 * 0) Background (theoretically shouldn't be changing)
	 * 1) Player (ppu sprites 0-9). These sprites all need some offset
	 * 2) Enemy Crystals (up to four at once, )
	 ******************************************************************/

	// //background scroll:
	// ppu.background_position.x = int32_t(-0.5f * player_at.x);
	// ppu.background_position.y = int32_t(-0.5f * player_at.y);

	// //player sprite:
	// ppu.sprites[0].x = int8_t(player_at.x);
	// ppu.sprites[0].y = int8_t(player_at.y);
	// ppu.sprites[0].index = 32;
	// ppu.sprites[0].attributes = 7;

	// //some other misc sprites:
	// for (uint32_t i = 1; i < 63; ++i) {
	// 	float amt = (i + 2.0f * background_fade) / 62.0f;
	// 	ppu.sprites[i].x = int8_t(0.5f * float(PPU466::ScreenWidth) + std::cos( 2.0f * M_PI * amt * 5.0f + 0.01f * player_at.x) * 0.4f * float(PPU466::ScreenWidth));
	// 	ppu.sprites[i].y = int8_t(0.5f * float(PPU466::ScreenHeight) + std::sin( 2.0f * M_PI * amt * 3.0f + 0.01f * player_at.y) * 0.4f * float(PPU466::ScreenWidth));
	// 	ppu.sprites[i].index = 32;
	// 	ppu.sprites[i].attributes = 6;
	// 	if (i % 2) ppu.sprites[i].attributes |= 0x80; //'behind' bit
	// }

	//--- actually draw ---
	ppu.draw(drawable_size);
}
