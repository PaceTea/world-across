#ifndef ALBW_CONFIG_H
#define ALBW_CONFIG_H

#include <stdint.h>

// The console's own settings, as the cfg service hands them to the game: language, region, country,
// the user's name, and the 3D camera parameters.
//
// They matter more than they look. The game builds its stereoscopic camera from the configuration
// block that describes the screen, and with that block left empty it divides by zero and uploads a
// projection matrix made of not-a-number -- which is a black screen, rendered correctly.
//
// The region comes from the card's own icon, because a setting the card was not made for is one the
// game may refuse. The language follows $LANG where the region offers that language, and
// ALBW_LANGUAGE overrides it; ALBW_USERNAME sets the name the game offers for the player.

// Fills `size` bytes of guest memory at `dest` with configuration block `block`. Returns the result
// code to report.
uint32_t albw_config_block(uint32_t block, uint32_t size, uint32_t dest);

// The region, numbered as SecureInfoGetRegion numbers it: 0 Japan, 1 the Americas, 2 Europe and
// Australia, 4 China, 5 Korea, 6 Taiwan.
uint32_t albw_config_region(void);

// The console's user name, up to ten plain characters, as the configuration block and the software
// keyboard both hand it out.
void albw_config_username(char out[11]);

#endif
