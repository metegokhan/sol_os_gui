#include "solar_os_blackjack.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"

#define BJ_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(BJ_STACK_SIZE);

static void bj_sfx_deal(void)
{
    solar_os_audio_play_tone(1600, 15, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void bj_sfx_chip(void)
{
    solar_os_audio_play_tone(900, 12, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void bj_sfx_win(void)
{
    solar_os_audio_play_tone(587, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(880, 80, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void bj_sfx_blackjack(void)
{
    solar_os_audio_play_tone(523, 40, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(659, 40, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(784, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1046, 120, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void bj_sfx_bust(void)
{
    solar_os_audio_play_tone(350, 60, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(220, 90, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

typedef struct {
    uint8_t suit; /* 0=S, 1=H, 2=D, 3=C */
    uint8_t rank; /* 1=A, 2..10, 11=J, 12=Q, 13=K */
} bj_card_t;

typedef enum {
    BJ_STATE_BETTING = 0,
    BJ_STATE_PLAYER_TURN,
    BJ_STATE_DEALER_TURN,
    BJ_STATE_ROUND_OVER,
} bj_game_state_t;

typedef struct {
    bj_card_t deck[52];
    int deck_idx;

    bj_card_t player_cards[10];
    int player_count;
    bj_card_t dealer_cards[10];
    int dealer_count;

    int chips;
    int current_bet;
    bj_game_state_t state;
    bool hide_dealer_hole;
    char status_msg[64];
    int hands_won;
    int hands_lost;
    int64_t dealer_step_us;
} blackjack_state_t;

static void *bj_state_ptr;
#define bj (*(blackjack_state_t *)bj_state_ptr)

static const char *bj_rank_names[14] = {"", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};

static void bj_shuffle(void)
{
    int k = 0;
    for (uint8_t s = 0; s < 4; s++) {
        for (uint8_t r = 1; r <= 13; r++) {
            bj.deck[k].suit = s;
            bj.deck[k].rank = r;
            k++;
        }
    }
    for (int i = 51; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        bj_card_t tmp = bj.deck[i];
        bj.deck[i] = bj.deck[j];
        bj.deck[j] = tmp;
    }
    bj.deck_idx = 0;
}

static bj_card_t bj_draw_card(void)
{
    if (bj.deck_idx >= 48) {
        bj_shuffle();
    }
    return bj.deck[bj.deck_idx++];
}

static int bj_calculate_hand_value(const bj_card_t *cards, int count, bool hide_hole)
{
    int val = 0;
    int aces = 0;
    int start = hide_hole ? 1 : 0;

    for (int i = start; i < count; i++) {
        int r = cards[i].rank;
        if (r == 1) {
            aces++;
            val += 11;
        } else if (r >= 10) {
            val += 10;
        } else {
            val += r;
        }
    }

    while (val > 21 && aces > 0) {
        val -= 10;
        aces--;
    }
    return val;
}

static void bj_deal_hand(void)
{
    if (bj.chips < bj.current_bet) {
        bj.current_bet = bj.chips > 0 ? bj.chips : 10;
        if (bj.chips <= 0) {
            bj.chips = 250; /* Free reload */
        }
    }

    bj.chips -= bj.current_bet;
    bj.player_count = 0;
    bj.dealer_count = 0;
    bj.hide_dealer_hole = true;
    bj_sfx_deal();

    bj.player_cards[bj.player_count++] = bj_draw_card();
    bj.dealer_cards[bj.dealer_count++] = bj_draw_card();
    bj.player_cards[bj.player_count++] = bj_draw_card();
    bj.dealer_cards[bj.dealer_count++] = bj_draw_card();

    int pval = bj_calculate_hand_value(bj.player_cards, bj.player_count, false);
    int dval = bj_calculate_hand_value(bj.dealer_cards, bj.dealer_count, false);

    if (pval == 21) {
        bj.hide_dealer_hole = false;
        bj.state = BJ_STATE_ROUND_OVER;
        if (dval == 21) {
            bj.chips += bj.current_bet;
            snprintf(bj.status_msg, sizeof(bj.status_msg), "BOTH BLACKJACK! Push (Tie).");
        } else {
            int win = bj.current_bet + (bj.current_bet * 3) / 2;
            bj.chips += win;
            bj.hands_won++;
            snprintf(bj.status_msg, sizeof(bj.status_msg), "BLACKJACK! You win $%d (3:2 payout)!", win);
            bj_sfx_blackjack();
        }
    } else {
        bj.state = BJ_STATE_PLAYER_TURN;
        snprintf(bj.status_msg, sizeof(bj.status_msg), "Your turn: [H] Hit | [S] Stand | [D] Double");
    }
}

static void bj_finish_round(void)
{
    bj.hide_dealer_hole = false;
    int pval = bj_calculate_hand_value(bj.player_cards, bj.player_count, false);
    int dval = bj_calculate_hand_value(bj.dealer_cards, bj.dealer_count, false);

    bj.state = BJ_STATE_ROUND_OVER;

    if (pval > 21) {
        bj.hands_lost++;
        snprintf(bj.status_msg, sizeof(bj.status_msg), "BUST! You went over 21. Lost $%d.", bj.current_bet);
        bj_sfx_bust();
    } else if (dval > 21) {
        int win = bj.current_bet * 2;
        bj.chips += win;
        bj.hands_won++;
        snprintf(bj.status_msg, sizeof(bj.status_msg), "DEALER BUSTS (%d)! You win $%d!", dval, win);
        bj_sfx_win();
    } else if (pval > dval) {
        int win = bj.current_bet * 2;
        bj.chips += win;
        bj.hands_won++;
        snprintf(bj.status_msg, sizeof(bj.status_msg), "YOU WIN! (%d vs %d) Won $%d!", pval, dval, win);
        bj_sfx_win();
    } else if (dval > pval) {
        bj.hands_lost++;
        snprintf(bj.status_msg, sizeof(bj.status_msg), "DEALER WINS (%d vs %d). Lost $%d.", dval, pval, bj.current_bet);
        bj_sfx_bust();
    } else {
        bj.chips += bj.current_bet;
        snprintf(bj.status_msg, sizeof(bj.status_msg), "PUSH! Tie (%d vs %d). Bet returned.", pval, dval);
    }
}

static void bj_draw_card_visual(solar_os_gfx_t *gfx, int x, int y, int w, int h, const bj_card_t *c, bool hidden)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, x, y, w, h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x + 2, y + 2, w - 4, h - 4);

    if (hidden) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, x + 4, y + 4, w - 8, h - 8);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, x + 14, y + 36, "?");
        return;
    }

    if (c == NULL) return;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, x + 5, y + 16, bj_rank_names[c->rank]);

    const char *s_txt = (c->suit == 0) ? "[S]" : ((c->suit == 1) ? "<H>" : ((c->suit == 2) ? "{D}" : "(C)"));
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, x + 5, y + 32, s_txt);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, x + (w / 2) - 6, y + (h / 2) + 6, bj_rank_names[c->rank]);
}

static void bj_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Top Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 15, "SOLAR CASINO BLACKJACK 21");

    char stats_s[48];
    snprintf(stats_s, sizeof(stats_s), "Bank: $%d | Bet: $%d", bj.chips, bj.current_bet);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t sw = solar_os_gfx_text_width(gfx, stats_s);
    solar_os_gfx_text(gfx, screen_w - (int)sw - 8, 15, stats_s);

    /* 2. Dealer Section */
    const int card_w = 46;
    const int card_h = 62;
    const int dealer_y = 32;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    int dval = bj_calculate_hand_value(bj.dealer_cards, bj.dealer_count, bj.hide_dealer_hole);
    char dval_s[32];
    if (bj.hide_dealer_hole) snprintf(dval_s, sizeof(dval_s), "DEALER: [%d + ?]", dval);
    else snprintf(dval_s, sizeof(dval_s), "DEALER: [%d]", dval);
    solar_os_gfx_text(gfx, 14, dealer_y + 16, dval_s);

    for (int i = 0; i < bj.dealer_count; i++) {
        bool hide = (i == 0 && bj.hide_dealer_hole);
        bj_draw_card_visual(gfx, 150 + (i * 38), dealer_y, card_w, card_h, &bj.dealer_cards[i], hide);
    }

    /* 3. Divider */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 10, 118, screen_w - 20, 2);

    /* 4. Player Section */
    const int player_y = 136;
    int pval = bj_calculate_hand_value(bj.player_cards, bj.player_count, false);
    char pval_s[32];
    snprintf(pval_s, sizeof(pval_s), "YOU: [%d]", pval);
    solar_os_gfx_text(gfx, 14, player_y + 16, pval_s);

    for (int i = 0; i < bj.player_count; i++) {
        bj_draw_card_visual(gfx, 150 + (i * 38), player_y, card_w, card_h, &bj.player_cards[i], false);
    }

    /* 5. Controls Overlay */
    const int ctrl_y = 216;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 10, ctrl_y, screen_w - 20, 52);

    if (bj.state == BJ_STATE_BETTING || bj.state == BJ_STATE_ROUND_OVER) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 20, ctrl_y + 20, "Place Bet: [UP/DOWN] Change ($10, $25, $50, $100) | [SPACE/ENTER] Deal Hand");
        char w_l[32]; snprintf(w_l, sizeof(w_l), "Wins: %d | Losses: %d", bj.hands_won, bj.hands_lost);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 20, ctrl_y + 40, w_l);
    } else if (bj.state == BJ_STATE_PLAYER_TURN) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 20, ctrl_y + 22, "[H] Hit  |  [S] Stand  |  [D] Double Down");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 20, ctrl_y + 42, "Dealer stands on 17+");
    } else if (bj.state == BJ_STATE_DEALER_TURN) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 20, ctrl_y + 32, "Dealer is drawing cards...");
    }

    /* 6. Footer */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 274, screen_w, 26);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 291, bj.status_msg);

    solar_os_gfx_present(gfx);
}

static esp_err_t bj_start(solar_os_context_t *ctx)
{
    memset(&bj, 0, sizeof(bj));
    bj.chips = 500;
    bj.current_bet = 25;
    bj.state = BJ_STATE_BETTING;
    bj_shuffle();
    snprintf(bj.status_msg, sizeof(bj.status_msg), "Welcome to Blackjack! Set bet and press [SPACE] to deal.");
    solar_os_context_set_graphics_active(ctx, true);
    bj_render(ctx);
    return ESP_OK;
}

static void bj_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool bj_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (bj.state == BJ_STATE_DEALER_TURN) {
            const int64_t now = esp_timer_get_time();
            if (bj.dealer_step_us != 0 && now >= bj.dealer_step_us) {
                int dval = bj_calculate_hand_value(bj.dealer_cards, bj.dealer_count, false);
                if (dval < 17 && bj.dealer_count < 8) {
                    bj.dealer_cards[bj.dealer_count++] = bj_draw_card();
                    bj.dealer_step_us = now + 400000;
                    bj_render(ctx);
                } else {
                    bj_finish_round();
                    bj_render(ctx);
                }
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }

        if (bj.state == BJ_STATE_BETTING || bj.state == BJ_STATE_ROUND_OVER) {
            if (ch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
                if (bj.current_bet < 200) {
                    bj.current_bet += (bj.current_bet >= 50 ? 25 : 15);
                    bj_sfx_chip();
                }
                bj_render(ctx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
                if (bj.current_bet > 10) {
                    bj.current_bet -= (bj.current_bet > 50 ? 25 : 15);
                    bj_sfx_chip();
                }
                bj_render(ctx);
                return true;
            }
            if (ch == '\r' || ch == '\n' || ch == ' ') {
                bj_deal_hand();
                bj_render(ctx);
                return true;
            }
        } else if (bj.state == BJ_STATE_PLAYER_TURN) {
            if (ch == 'h' || ch == 'H') {
                /* Hit */
                bj_sfx_deal();
                bj.player_cards[bj.player_count++] = bj_draw_card();
                int pval = bj_calculate_hand_value(bj.player_cards, bj.player_count, false);
                if (pval > 21) {
                    bj_finish_round();
                } else if (pval == 21) {
                    bj.state = BJ_STATE_DEALER_TURN;
                    bj.hide_dealer_hole = false;
                    bj.dealer_step_us = esp_timer_get_time() + 400000;
                }
                bj_render(ctx);
                return true;
            }
            if (ch == 's' || ch == 'S' || ch == ' ' || ch == '\r' || ch == '\n') {
                /* Stand */
                bj.state = BJ_STATE_DEALER_TURN;
                bj.hide_dealer_hole = false;
                bj.dealer_step_us = esp_timer_get_time() + 400000;
                bj_render(ctx);
                return true;
            }
            if (ch == 'd' || ch == 'D') {
                /* Double down */
                if (bj.chips >= bj.current_bet) {
                    bj.chips -= bj.current_bet;
                    bj.current_bet *= 2;
                    bj_sfx_deal();
                    bj.player_cards[bj.player_count++] = bj_draw_card();
                    bj.state = BJ_STATE_DEALER_TURN;
                    bj.hide_dealer_hole = false;
                    bj.dealer_step_us = esp_timer_get_time() + 400000;
                    bj_render(ctx);
                    return true;
                }
            }
        }
    }

    return false;
}

const solar_os_app_t solar_os_blackjack_app = {
    .name = "blackjack",
    .summary = "classic casino Blackjack 21 card game",
    .flags = 0,
    .start = bj_start,
    .stop = bj_stop,
    .event = bj_event,
    .state_slot = &bj_state_ptr,
    .state_size = sizeof(blackjack_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 50U,
    .worker_stack_bytes = BJ_STACK_SIZE,
};
