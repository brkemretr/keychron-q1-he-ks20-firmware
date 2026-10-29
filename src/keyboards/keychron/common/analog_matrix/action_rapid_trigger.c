/* Copyright 2024 @ Keychron (https://www.keychron.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "analog_matrix.h"

bool rapid_trigger_action(analog_key_t *key) {
    bool    changed          = false;
    int8_t  update_rapid_pts = 0;
    uint8_t eff_sen          = key->rpd_trig_sen;
    uint8_t eff_rls          = key->rpd_trig_sen_rls;

    // In deeper travel (> 2.0mm) where Hall sensor approaches rail saturation,
    // enforce a minimum 0.30mm (18 units) rapid window to prevent spring/saturation chatter
    if (key->travel > 20 * TRAVEL_SCALE) {
        if (eff_sen < 3 * TRAVEL_SCALE) eff_sen = 3 * TRAVEL_SCALE;
        if (eff_rls < 3 * TRAVEL_SCALE) eff_rls = 3 * TRAVEL_SCALE;
    }

    switch (key->state) {
        case AKS_REGULAR_RELEASED:
            // First actuation
            if (key->travel >= key->regular.actn_pt) {
                key->state       = AKS_REGULAR_PRESSED;
                changed          = true;
                update_rapid_pts = 1;
            }
            break;

        case AKS_REGULAR_PRESSED:
            // Key releasing: full deactuation or rapid release outside bottom deadzone
            if (key->travel == 0 || key->travel < key->regular.deactn_pt) {
                key->state = AKS_REGULAR_RELEASED;
                changed    = true;
            } else if (key->travel <= key->rapid.deactn_pt && key->travel < BOTTOM_DEAD_ZONE * TRAVEL_SCALE - eff_rls) {
                key->state       = AKS_RAPID_RELEASED;
                changed          = true;
                update_rapid_pts = -1;
            }
            // Continue pressing deeper
            else if (key->travel > key->rapid.actn_pt) {
                update_rapid_pts = 1;
            }
            break;

        case AKS_RAPID_RELEASED:
            // Continue releasing into upper zone
            if (key->travel == 0 || key->travel <= key->regular.deactn_pt) {
                key->state = AKS_REGULAR_RELEASED;
            }
            // Press again (must be both >= rapid.actn_pt and >= regular.actn_pt)
            else if (key->travel >= key->rapid.actn_pt && key->travel >= key->regular.actn_pt) {
                key->state       = AKS_RAPID_PRESSED;
                changed          = true;
                update_rapid_pts = 1;
            } else if (key->travel < key->rapid.deactn_pt) {
                update_rapid_pts = -1;
            }
            break;

        case AKS_RAPID_PRESSED:
            // Key releasing
            if (key->travel > FULL_TRAVEL_UNIT * TRAVEL_SCALE) {
                break;
            }
            if (key->travel == 0 || key->travel < key->regular.deactn_pt) {
                key->state = AKS_REGULAR_RELEASED;
                changed    = true;
            } else if (key->travel <= key->rapid.deactn_pt && key->travel < BOTTOM_DEAD_ZONE * TRAVEL_SCALE - eff_rls) {
                key->state       = AKS_RAPID_RELEASED;
                changed          = true;
                update_rapid_pts = -1;
            }
            // Continue pressing deeper
            else if (key->travel > key->rapid.actn_pt) {
                update_rapid_pts = 1;
            }
            break;

        default:
            break;
    }

    if (update_rapid_pts) {
        if (update_rapid_pts > 0) {
            key->rapid.deactn_pt = (key->travel > eff_rls) ? (key->travel - eff_rls) : 0;
            key->rapid.actn_pt   = key->travel;
        } else {
            key->rapid.deactn_pt = key->travel;
            key->rapid.actn_pt   = key->travel + eff_sen;
        }
    }

    return changed;
}
