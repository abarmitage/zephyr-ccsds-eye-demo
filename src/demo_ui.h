/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_UI_H
#define CCSDS_EYE_DEMO_UI_H

#include <stdint.h>

void demo_ui_init(void);
void demo_ui_handle_key(uint16_t code, int32_t value);
void demo_ui_tick(uint32_t now_ms);

#endif /* CCSDS_EYE_DEMO_UI_H */

