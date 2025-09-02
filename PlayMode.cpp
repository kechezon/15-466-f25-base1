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
void process_tiles() {
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
	// const uint64_t true_pixel_count = (64*48);
	glm::uvec2 *spritesheet_size = new glm::uvec2(64, 48);
	std::vector< glm::u8vec4 > raw_tile_pixels;

	// ColoredTile holds color info rather than palette index info (we'll use it to construct the palette info for the tile)
	std::array< ColoredTile, tile_count > colored_tiles;
	load_png("assets/spritesheet.png", spritesheet_size, &raw_tile_pixels, OriginLocation::UpperLeftOrigin);

	// 2) Index math
	for (uint64_t r = 0; r < spritesheet_dimensions / 8; r++) {
		// 8-pixel row by 8-pixel row, assign to the right tile
		std::array< glm::u8vec4, 8 > pixel_row;
		
		// go across the pixels and get the row
		for (int p = 0; p < 8; p++) {
			pixel_row[p] = raw_tile_pixels[(8 * r) + p];
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

	// DEBUG print tiles
	// for (uint64_t i = 0; i < tile_count; i++) {
	// 	ColoredTile tile = colored_tiles[i];
	// 	printf("TILE %llu:\n", i);
	// 	for (uint64_t r = 0; r < 8; r++) {
	// 		printf("{");
	// 		std::array<glm::u8vec4, 8> row = tile[r];
	// 		for (uint64_t c = 0; c < 8; c++) {
	// 			printf("(%x, %x, %x, %x) ~ ", row[c].x, row[c].y, row[c].z, row[c].w);
	// 		}
	// 		printf("}\n");
	// 	}

	// 	printf("\n");
	// }

	// 3) Palette indices
	// mapping of rgba -> number is a bucket
	std::array<PaletteBucket, tile_count> palette_buckets;
	std::unordered_map< uint64_t, uint64_t > tile_palette_map = {}; // used to assign palette indices (color index to palette index)
	uint64_t buckets_made = 0;
	for (uint64_t t = 0; t < tile_count; t++) {
		ColoredTile tile = colored_tiles[t];

		// a) get the colors of the tile
		std::unordered_set<uint32_t> colors; 
		for (auto row : tile) {
			for (uint8_t p = 0; p < 8; p++)
				colors.emplace((row[p].x << 24) + (row[p].y << 16) + (row[p].z << 8) + row[p].w);
		}
		std::vector<glm::u8vec4> color_keys = {};
		for (uint32_t color : colors) {
			color_keys.emplace_back(glm::u8vec4((color >> 24) & 0xff,
									 			(color >> 16) & 0xff,
									 			(color >> 8) & 0xff,
									  			 color & 0xff));
		}

		// DEBUG what colors were found?
		// printf("Found %llu colors in tile %llu: { ~ ", color_keys.size(), t);
		// for (int i = 0; i < color_keys.size(); i++) {
		// 	printf("(%u, %u, %u, %u) ~ ", color_keys[i].x, color_keys[i].y, color_keys[i].z, color_keys[i].w);
		// }
		// printf("}\n");

		// b) determine which PaletteBucket it goes in, and add to the tile_palette_map
		//	  (update buckets as necessary)

		// to compare palettes against the current
		// std::vector<PaletteBucket> palettes;
		// for (auto [key, bucket] : tile_palette_map) {
		// 	palettes.emplace_back(bucket);
		// }
		
		// compare `colors` to each palette
		bool bucket_found = false;
		for (uint64_t p = 0; p < palette_buckets.size(); p++) {
			PaletteBucket bucket = palette_buckets[p];

			if (bucket.size() > 0) {
				std::vector<glm::u8vec4> cmpPalette = {};
				for (auto color_vec : bucket) {
					cmpPalette.emplace_back(color_vec);
				}

				if (palette_match(color_keys, cmpPalette)) // first palette to match, map
				{
					bucket_found = true;
					// the larger palette is what the tiles are mapped to
					if (cmpPalette.size() < colors.size())
					{
						// update the bucket with the colors in `colors`
						// for (auto color : colors) {
							palette_buckets[p] = color_keys;
							// uint32_t color_bytes = (color[0] << 24) + (color[1] << 16) + (color[2] << 8) + color[3];
							// bucket[color_bytes] = (uint8_t)bucket.size();
				// 		}
					}
					tile_palette_map[t] = p;
					break;
				}
			}
		}

		if (!bucket_found) {
			palette_buckets[buckets_made] = color_keys;
			tile_palette_map[t] = buckets_made;
			buckets_made++;
		}
	}

	for (uint64_t b = 0; b < buckets_made; b++) {
		PaletteBucket bucket = palette_buckets[b];
		// c) Go through each bucket and reassign numbers
		// get keys
		// std::vector<glm::u8vec4> keys;
		// for (auto [color, num] : bucket) {
		// 	keys.emplace_back(color);
		// }

		// sort and remap
		for (uint8_t i = 0; i < 3; i++) {
			for (uint8_t j = i; j < 4; j++) {
				if (color_compare(bucket[j], bucket[i]) < 0 || (bucket[j].x == 0xee && bucket[j].y == 0xee && bucket[j].z == 0xee)) {
					glm::uvec4 temp = bucket[i];
					bucket[i] = bucket[j];
					bucket[j] = temp;
				}
			}
			// uint32_t color_bytes = (keys[i][3] << 24) + (keys[i][2] << 16) + (keys[i][1] << 8) + keys[i][0];
			// bucket[color_bytes] = i;
		}
	}


	for (uint64_t t = 0; t < tile_count; t++) {
		// static_assert(tile_palette_map.contains(tile_colors[t]), "Colored tile %lu was added to the tp map");
		// d) construct tile
		ColoredTile tile = colored_tiles[t];
		uint64_t palette_index = tile_palette_map[t];
		// printf("Tile %zu -> Palette %zu.\n", t, palette_index);
		PaletteBucket palette_bucket = palette_buckets[palette_index]; // color->int

		for (uint64_t y = 0; y < 8; y++) {
			std::array< glm::u8vec4, 8 > tile_row = tile[y];
			uint8_t row_bit_0 = 0;
			uint8_t row_bit_1 = 0;
			for (uint64_t x = 0; x < 8; x++) {
				glm::u8vec4 color = tile_row[x];
				// uint32_t color_bytes = (tile_row[x][3] << 24) + (tile_row[x][2] << 16) + (tile_row[x][1] << 8) + tile_row[x][0];

				uint8_t palette_idx = 0;
				for (uint8_t i = 0; i < palette_bucket.size(); i++)
					if (color_compare(color, palette_bucket[i]) == 0)
						palette_idx = i;
				// printf("Mapped color (%zu, %zu) -> palette index %u\n", x, y, palette_idx);

				// (x,y) -> color -> palette_idx (from PaletteBucket)
				row_bit_0 += (palette_idx % 2) << (7 - x);
				row_bit_1 += (palette_idx / 2) << (7 - x);
			}
			ppu.tile_table[t].bit0[y] = row_bit_0;
			ppu.tile_table[t].bit1[y] = row_bit_1;
		}
		// printf("\n");
	}

}
void process_palettes() {
	/*********************************************************************************
	 * PALETTES
	 * Palettes are also pngs, use load_png to convert to color format.
	 * Assumes palette is already sorted
	 * (with the exception of "eeeeee" leading if present,
	 *  as that marks transparency)
	 *********************************************************************************/
	glm::uvec2 *palette_sheet_size = new glm::uvec2(4, 8);
	std::vector< glm::u8vec4 > palette_data;

	load_png("assets/palettes.png", palette_sheet_size, &palette_data, OriginLocation::UpperLeftOrigin);
	for (uint64_t i = 0; i < palette_data.size(); i++) {
		uint64_t pal_tbl_idx = i / 4;
		uint64_t pal_idx = i % 4;

		if (pal_idx == 0 && palette_data[i][0] == 0xee && palette_data[i][1] == 0xee && palette_data[i][2] == 0xee)
			ppu.palette_table[pal_tbl_idx][0] = {0x00, 0x00, 0x00, 0x00};
		else
			ppu.palette_table[pal_tbl_idx][pal_idx] = palette_data[i];
	}

}
void create_player_sprites() {
	for (uint8_t i = 0; i < 4; i++)
		playerSprites[i].index = i; // head, body, gemstar, reticle

	playerSprites[0].attributes = 0x00;
	playerSprites[1].attributes = 0x00;
	playerSprites[2].attributes = 0x01;
	playerSprites[3].attributes = 0x02;
}

PlayMode::PlayMode() {
	//Asset Pipeline
	process_tiles();
	process_palettes();


	/**********************************
	 * Sprite (entity) lists
	 **********************************/
	create_player_sprites();
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

	// constexpr float PlayerSpeed = 30.0f;
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
		player.gameObject.position[1] = (float) GROUND_LEVEL + player.gameObject.height_radius[1];
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
		ppu.background[t] = 0b0000000100011100; //0x011C;
	}
	for (uint32_t t = bg_size - 256; t < bg_size; t++) {
		ppu.background[t] = 0b0000000100011101; //0x011D;
	}

	playerSprites[0].x = uint8_t(player.gameObject.position[0] - player.gameObject.width_radius[0]);
	playerSprites[0].y = uint8_t(player.gameObject.position[1] + 1);
	playerSprites[1].x = uint8_t(player.gameObject.position[0] - player.gameObject.width_radius[0]);
	playerSprites[1].y = uint8_t(player.gameObject.position[1] - player.gameObject.height_radius[1]);

	// set to playerSprites 2 based on active
	// set to playerSprites 3 to mouse

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
