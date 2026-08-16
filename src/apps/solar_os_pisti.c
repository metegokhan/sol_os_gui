#include "solar_os_pisti.h"

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

#define PISTI_STACK_SIZE 10240
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PISTI_STACK_SIZE);

static void pst_sfx_card_deal(void)
{
    solar_os_audio_play_tone(1800, 15, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void pst_sfx_card_play(void)
{
    solar_os_audio_play_tone(1200, 18, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void pst_sfx_capture(void)
{
    solar_os_audio_play_tone(600, 25, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(800, 35, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void pst_sfx_pisti(void)
{
    solar_os_audio_play_tone(659, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(880, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1174, 90, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void pst_sfx_double_pisti(void)
{
    solar_os_audio_play_tone(523, 40, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(659, 40, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(784, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(1046, 110, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

static void pst_sfx_win(void)
{
    solar_os_audio_play_tone(440, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(554, 50, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(659, 70, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    solar_os_audio_play_tone(880, 120, SOLAR_OS_AUDIO_VOLUME_GLOBAL);
}

/* Suits: 0=Spades(♠), 1=Hearts(♥), 2=Diamonds(♦), 3=Clubs(♣)
 * Ranks: 1=A, 2..10, 11=J, 12=Q, 13=K
 */
typedef struct {
    uint8_t suit;
    uint8_t rank;
} card_t;

typedef struct {
    card_t deck[52];
    int deck_idx;

    /* Board Pile */
    card_t pile[52];
    int pile_count;

    /* Hands */
    card_t player_hand[4];
    int player_hand_count;
    card_t cpu_hand[4];
    int cpu_hand_count;

    /* Captures & Points */
    int player_captured_count;
    int cpu_captured_count;
    int player_pisti_count;
    int cpu_pisti_count;
    int player_double_pisti;
    int cpu_double_pisti;

    int total_player_score;
    int total_cpu_score;

    /* State */
    bool player_turn;
    bool vs_cpu;
    int deal_round; /* 1 to 6 */
    int selected_card; /* 0 to hand_count - 1 */
    int last_capturer; /* 0 = player, 1 = cpu */
    bool game_over;
    bool round_over;
    bool last_was_pisti;

    char status_msg[64];
    int64_t cpu_play_at_us;
} pisti_state_t;

static void *pisti_state_ptr;
#define pst (*(pisti_state_t *)pisti_state_ptr)

static const char *suit_symbols[4] = {"S", "H", "D", "C"}; /* Spades, Hearts, Diamonds, Clubs */
static const char *rank_names[14] = {"", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};

static void pisti_shuffle_deck(void)
{
    int k = 0;
    for (uint8_t s = 0; s < 4; s++) {
        for (uint8_t r = 1; r <= 13; r++) {
            pst.deck[k].suit = s;
            pst.deck[k].rank = r;
            k++;
        }
    }
    for (int i = 51; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        card_t tmp = pst.deck[i];
        pst.deck[i] = pst.deck[j];
        pst.deck[j] = tmp;
    }
    pst.deck_idx = 0;
}

static void pisti_deal_hands(void)
{
    pst.player_hand_count = 4;
    pst.cpu_hand_count = 4;
    for (int i = 0; i < 4; i++) {
        pst.player_hand[i] = pst.deck[pst.deck_idx++];
        pst.cpu_hand[i] = pst.deck[pst.deck_idx++];
    }
    pst.deal_round++;
    pst.selected_card = 0;
    pst_sfx_card_deal();
}

static void pisti_start_new_game(void)
{
    pisti_shuffle_deck();
    pst.pile_count = 4;
    for (int i = 0; i < 4; i++) {
        pst.pile[i] = pst.deck[pst.deck_idx++];
    }

    pst.deal_round = 0;
    pst.player_captured_count = 0;
    pst.cpu_captured_count = 0;
    pst.player_pisti_count = 0;
    pst.cpu_pisti_count = 0;
    pst.player_double_pisti = 0;
    pst.cpu_double_pisti = 0;
    pst.last_capturer = -1;
    pst.game_over = false;
    pst.round_over = false;
    pst.last_was_pisti = false;
    pst.player_turn = true;

    pisti_deal_hands();
    snprintf(pst.status_msg, sizeof(pst.status_msg), "New Pişti Match! Round 1/6. Your turn.");
}

static int pisti_calculate_card_points(const card_t *cards, int count)
{
    int pts = 0;
    for (int i = 0; i < count; i++) {
        if (cards[i].rank == 1) pts += 1; /* As = 1 */
        else if (cards[i].rank == 11) pts += 1; /* Vale = 1 */
        else if (cards[i].suit == 2 && cards[i].rank == 10) pts += 3; /* 10♦ Karo 10 = 3 */
        else if (cards[i].suit == 3 && cards[i].rank == 2) pts += 2; /* 2♣ Sinek 2 = 2 */
    }
    return pts;
}

static void pisti_end_game(void)
{
    /* Remaining board cards go to the last capturer */
    if (pst.pile_count > 0 && pst.last_capturer != -1) {
        if (pst.last_capturer == 0) {
            pst.player_captured_count += pst.pile_count;
        } else {
            pst.cpu_captured_count += pst.pile_count;
        }
        pst.pile_count = 0;
    }

    int p_score = (pst.player_pisti_count * 10) + (pst.player_double_pisti * 20);
    int c_score = (pst.cpu_pisti_count * 10) + (pst.cpu_double_pisti * 20);

    /* Most cards bonus (+3) */
    if (pst.player_captured_count > pst.cpu_captured_count) p_score += 3;
    else if (pst.cpu_captured_count > pst.player_captured_count) c_score += 3;

    pst.total_player_score += p_score;
    pst.total_cpu_score += c_score;
    pst.game_over = true;

    if (p_score > c_score) {
        snprintf(pst.status_msg, sizeof(pst.status_msg), "YOU WON! (+%d pts vs CPU %d pts)", p_score, c_score);
        pst_sfx_win();
    } else if (c_score > p_score) {
        snprintf(pst.status_msg, sizeof(pst.status_msg), "CPU WON! (%d pts vs %d pts)", c_score, p_score);
    } else {
        snprintf(pst.status_msg, sizeof(pst.status_msg), "TIED GAME! (%d - %d)", p_score, c_score);
    }
}

static void pisti_play_card(int hand_idx, bool is_player)
{
    card_t card;
    if (is_player) {
        if (hand_idx < 0 || hand_idx >= pst.player_hand_count) return;
        card = pst.player_hand[hand_idx];
        for (int i = hand_idx; i < pst.player_hand_count - 1; i++) {
            pst.player_hand[i] = pst.player_hand[i + 1];
        }
        pst.player_hand_count--;
        if (pst.selected_card >= pst.player_hand_count && pst.selected_card > 0) {
            pst.selected_card--;
        }
    } else {
        if (hand_idx < 0 || hand_idx >= pst.cpu_hand_count) return;
        card = pst.cpu_hand[hand_idx];
        for (int i = hand_idx; i < pst.cpu_hand_count - 1; i++) {
            pst.cpu_hand[i] = pst.cpu_hand[i + 1];
        }
        pst.cpu_hand_count--;
    }

    bool captured = false;
    bool is_pisti = false;
    bool is_double = false;

    if (pst.pile_count > 0) {
        card_t top = pst.pile[pst.pile_count - 1];

        if (card.rank == top.rank) {
            /* Rank match capture! */
            captured = true;
            if (pst.pile_count == 1) {
                is_pisti = true;
                if (card.rank == 11) is_double = true; /* Jack on Jack */
            }
        } else if (card.rank == 11) {
            /* Jack capture! */
            captured = true;
        }
    }

    if (captured) {
        int won_count = pst.pile_count + 1;
        pst.pile_count = 0;
        pst.last_capturer = is_player ? 0 : 1;

        if (is_player) {
            pst.player_captured_count += won_count;
            if (is_double) {
                pst.player_double_pisti++;
                snprintf(pst.status_msg, sizeof(pst.status_msg), "★ DOUBLE PISTI with JACK (+20 PTS)! ★");
                pst_sfx_double_pisti();
            } else if (is_pisti) {
                pst.player_pisti_count++;
                snprintf(pst.status_msg, sizeof(pst.status_msg), "★ PISTI (+10 PTS)! ★");
                pst_sfx_pisti();
            } else {
                snprintf(pst.status_msg, sizeof(pst.status_msg), "Captured %d cards with %s!", won_count, rank_names[card.rank]);
                pst_sfx_capture();
            }
        } else {
            pst.cpu_captured_count += won_count;
            if (is_double) {
                pst.cpu_double_pisti++;
                snprintf(pst.status_msg, sizeof(pst.status_msg), "CPU made DOUBLE PISTI! (+20 pts)");
                pst_sfx_double_pisti();
            } else if (is_pisti) {
                pst.cpu_pisti_count++;
                snprintf(pst.status_msg, sizeof(pst.status_msg), "CPU made PISTI! (+10 pts)");
                pst_sfx_pisti();
            } else {
                snprintf(pst.status_msg, sizeof(pst.status_msg), "CPU captured %d cards with %s!", won_count, rank_names[card.rank]);
                pst_sfx_capture();
            }
        }
    } else {
        /* Add to pile */
        pst.pile[pst.pile_count++] = card;
        pst_sfx_card_play();
        if (!is_player) {
            snprintf(pst.status_msg, sizeof(pst.status_msg), "CPU played %s of %s. Your turn.",
                     rank_names[card.rank], suit_symbols[card.suit]);
        }
    }

    /* Switch Turn */
    pst.player_turn = !pst.player_turn;

    /* Check deal end */
    if (pst.player_hand_count == 0 && pst.cpu_hand_count == 0) {
        if (pst.deal_round < 6) {
            pisti_deal_hands();
            snprintf(pst.status_msg, sizeof(pst.status_msg), "Dealt Round %d/6!", pst.deal_round);
        } else {
            pisti_end_game();
        }
    } else if (!pst.player_turn && pst.vs_cpu && !pst.game_over) {
        pst.cpu_play_at_us = esp_timer_get_time() + 450000; /* 450ms CPU delay */
    }
}

/* Intelligent Pişti AI */
static void pisti_cpu_play(void)
{
    if (pst.player_turn || pst.cpu_hand_count == 0 || pst.game_over) return;

    card_t top = {0, 0};
    bool has_top = pst.pile_count > 0;
    if (has_top) top = pst.pile[pst.pile_count - 1];

    int best_idx = 0;
    int best_score = -999;

    for (int i = 0; i < pst.cpu_hand_count; i++) {
        card_t c = pst.cpu_hand[i];
        int score = 0;

        if (has_top && c.rank == top.rank) {
            /* Capture with match */
            score = 100;
            if (pst.pile_count == 1) score += 200; /* PISTI! */
            if (c.rank == 1) score += 20; /* Capture As */
            if (c.suit == 2 && c.rank == 10) score += 30; /* 10♦ */
        } else if (c.rank == 11) {
            /* Jack */
            if (pst.pile_count >= 3) score = 80 + pst.pile_count * 10;
            else if (has_top && (top.rank == 1 || (top.suit == 2 && top.rank == 10))) score = 90;
            else score = -50; /* Save Jack if pile is small */
        } else {
            /* Throw away card */
            score = 10 - c.rank; /* Prefer throwing lower non-point ranks */
            if (c.rank == 1) score -= 40; /* Avoid throwing Ace */
            if (c.suit == 2 && c.rank == 10) score -= 60; /* Avoid throwing 10♦ */
            if (c.suit == 3 && c.rank == 2) score -= 50; /* Avoid throwing 2♣ */
        }

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    pisti_play_card(best_idx, false);
}

static void pisti_draw_card_box(solar_os_gfx_t *gfx, int x, int y, int w, int h, const card_t *c, bool is_selected, bool face_down)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, x, y, w, h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x + 2, y + 2, w - 4, h - 4);

    if (face_down) {
        /* Decorative card back */
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, x + 4, y + 4, w - 8, h - 8);
        for (int i = 6; i < h - 6; i += 6) {
            solar_os_gfx_fill_rect(gfx, x + 6, y + i, w - 12, 1);
        }
        return;
    }

    if (c == NULL) return;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* Rank Text Top-Left */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, x + 5, y + 16, rank_names[c->rank]);

    /* Suit Badge */
    const char *s_txt = (c->suit == 0) ? "[S]" : ((c->suit == 1) ? "<H>" : ((c->suit == 2) ? "{D}" : "(C)"));
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, x + 6, y + 32, s_txt);

    /* Center Emblem */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, x + (w / 2) - 6, y + (h / 2) + 6, rank_names[c->rank]);

    if (is_selected) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, x - 2, y - 2, w + 4, h + 4);
        solar_os_gfx_rect(gfx, x - 1, y - 1, w + 2, h + 2);
    }
}

static void pisti_draw(solar_os_gfx_t *gfx)
{
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    /* 1. Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 15, "SOLAROS PISTI CARD GAME");

    char hdr_info[48];
    snprintf(hdr_info, sizeof(hdr_info), "Round: %d/6 | %s",
             pst.deal_round, pst.vs_cpu ? "vs CPU" : "2-Player");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t hw = solar_os_gfx_text_width(gfx, hdr_info);
    solar_os_gfx_text(gfx, screen_w - (int)hw - 8, 15, hdr_info);

    /* 2. CPU Hand (Top) */
    const int cpu_y = 30;
    const int card_w = 46;
    const int card_h = 62;

    for (int i = 0; i < pst.cpu_hand_count; i++) {
        int cx = 110 + (i * 50);
        pisti_draw_card_box(gfx, cx, cpu_y, card_w, card_h, NULL, false, true);
    }

    /* CPU Stats Box (Top Right) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 310, 28, 82, 66);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 316, 44, "CPU STATS");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char cpu_s[32];
    snprintf(cpu_s, sizeof(cpu_s), "Cards: %d", pst.cpu_captured_count);
    solar_os_gfx_text(gfx, 316, 60, cpu_s);
    snprintf(cpu_s, sizeof(cpu_s), "Pisti: %d", pst.cpu_pisti_count + pst.cpu_double_pisti * 2);
    solar_os_gfx_text(gfx, 316, 76, cpu_s);
    snprintf(cpu_s, sizeof(cpu_s), "Score: %d", pst.total_cpu_score);
    solar_os_gfx_text(gfx, 316, 90, cpu_s);

    /* 3. Center Table Pile */
    const int table_y = 106;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 10, table_y, 290, 80);

    if (pst.pile_count == 0) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 100, table_y + 45, "[ TABLE IS EMPTY ]");
    } else {
        /* Layered pile visualization */
        for (int i = 0; i < pst.pile_count - 1 && i < 4; i++) {
            pisti_draw_card_box(gfx, 120 + (i * 6), table_y + 8, card_w, card_h, NULL, false, true);
        }
        /* Top Card */
        int top_x = 120 + ((pst.pile_count > 4 ? 4 : pst.pile_count - 1) * 6);
        pisti_draw_card_box(gfx, top_x, table_y + 8, card_w, card_h, &pst.pile[pst.pile_count - 1], false, false);

        char pile_s[32];
        snprintf(pile_s, sizeof(pile_s), "%d cards in pile", pst.pile_count);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 20, table_y + 45, pile_s);
    }

    /* Player Stats Box (Center Right) */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 310, 106, 82, 80);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 316, 122, "YOU");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char ply_s[32];
    snprintf(ply_s, sizeof(ply_s), "Cards: %d", pst.player_captured_count);
    solar_os_gfx_text(gfx, 316, 140, ply_s);
    snprintf(ply_s, sizeof(ply_s), "Pisti: %d", pst.player_pisti_count + pst.player_double_pisti * 2);
    solar_os_gfx_text(gfx, 316, 158, ply_s);
    snprintf(ply_s, sizeof(ply_s), "Score: %d", pst.total_player_score);
    solar_os_gfx_text(gfx, 316, 176, ply_s);

    /* 4. Player Hand (Bottom) */
    const int player_y = 196;
    for (int i = 0; i < pst.player_hand_count; i++) {
        int px = 80 + (i * 56);
        bool is_sel = (i == pst.selected_card && pst.player_turn && !pst.game_over);
        pisti_draw_card_box(gfx, px, is_sel ? player_y - 6 : player_y, card_w, card_h, &pst.player_hand[i], is_sel, false);
    }

    /* 5. Footer Status Bar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 274, screen_w, 26);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 291, pst.status_msg);

    solar_os_gfx_present(gfx);
}

static esp_err_t pisti_start(solar_os_context_t *ctx)
{
    memset(&pst, 0, sizeof(pst));
    pst.vs_cpu = true;
    pisti_start_new_game();
    solar_os_context_set_graphics_active(ctx, true);
    pisti_draw(solar_os_context_gfx(ctx));
    return ESP_OK;
}

static void pisti_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool pisti_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);

    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!pst.player_turn && pst.vs_cpu && !pst.game_over) {
            const int64_t now = esp_timer_get_time();
            if (pst.cpu_play_at_us != 0 && now >= pst.cpu_play_at_us) {
                pst.cpu_play_at_us = 0;
                pisti_cpu_play();
                pisti_draw(gfx);
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

        if (ch == 'r' || ch == 'R') {
            pisti_start_new_game();
            pisti_draw(gfx);
            return true;
        }

        if (ch == 'm' || ch == 'M') {
            pst.vs_cpu = !pst.vs_cpu;
            snprintf(pst.status_msg, sizeof(pst.status_msg), "Mode: %s", pst.vs_cpu ? "vs CPU" : "2-Player Pass & Play");
            pisti_draw(gfx);
            return true;
        }

        if (pst.game_over) {
            if (ch == '\r' || ch == '\n' || ch == ' ') {
                pisti_start_new_game();
                pisti_draw(gfx);
            }
            return true;
        }

        /* Player Controls */
        if (pst.player_turn) {
            if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A') {
                if (pst.selected_card > 0) pst.selected_card--;
                pisti_draw(gfx);
                return true;
            }
            if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D') {
                if (pst.selected_card + 1 < pst.player_hand_count) pst.selected_card++;
                pisti_draw(gfx);
                return true;
            }
            if (ch >= '1' && ch <= '4') {
                int idx = ch - '1';
                if (idx < pst.player_hand_count) {
                    pst.selected_card = idx;
                    pisti_play_card(pst.selected_card, true);
                    pisti_draw(gfx);
                    return true;
                }
            }
            if (ch == '\r' || ch == '\n' || ch == ' ') {
                pisti_play_card(pst.selected_card, true);
                pisti_draw(gfx);
                return true;
            }
        }
    }

    return false;
}

const solar_os_app_t solar_os_pisti_app = {
    .name = "pisti",
    .summary = "classic Turkish card game Pişti with AI",
    .flags = 0,
    .start = pisti_start,
    .stop = pisti_stop,
    .event = pisti_event,
    .state_slot = &pisti_state_ptr,
    .state_size = sizeof(pisti_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 50U,
    .worker_stack_bytes = PISTI_STACK_SIZE,
};
