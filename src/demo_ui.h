/* SPDX-License-Identifier: Apache-2.0 */

#ifndef CCSDS_EYE_DEMO_UI_H
#define CCSDS_EYE_DEMO_UI_H

#include <stdint.h>

#include "demo_service.h"

struct device;

void demo_ui_init(void);
void demo_ui_handle_key(uint16_t code, int32_t value);
void demo_ui_handle_event(const struct demo_ui_event *event);
void demo_ui_tick(uint32_t now_ms);
bool demo_ui_image_active(void);
int demo_ui_render_image(const struct device *display);
void demo_ui_toggle_show(void);
void demo_ui_prepare_action(void);
void demo_ui_report_busy(void);

#endif /* CCSDS_EYE_DEMO_UI_H */
