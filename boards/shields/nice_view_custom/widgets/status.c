/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

LV_IMG_DECLARE(flora_00);

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

/* 오브젝트 child 인덱스 */
#define OBJ_IDX_IMAGE       0
#define OBJ_IDX_BAT_OUTLINE 1
#define OBJ_IDX_BAT_FILL    2
#define OBJ_IDX_BAT_TIP     3
#define OBJ_IDX_BOLT_LABEL  4
#define OBJ_IDX_CONN_LABEL  5

/* 배터리 위치 (화면 우상단 기준) */
#define BAT_X   34
#define BAT_Y    2
#define BAT_W   28
#define BAT_H   12
#define BAT_TIP_W  3
#define BAT_TIP_H  6

/* 연결 아이콘 위치 */
#define CONN_X  50
#define CONN_Y   0

static void update_battery(lv_obj_t *widget, const struct status_state *state) {
    lv_obj_t *fill      = lv_obj_get_child(widget, OBJ_IDX_BAT_FILL);
    lv_obj_t *bolt_label = lv_obj_get_child(widget, OBJ_IDX_BOLT_LABEL);

    /* 배터리 잔량에 따라 채움 너비 조정 */
    int fill_w = (state->battery * (BAT_W - 4)) / 100;
    if (fill_w < 2) fill_w = 2;
    lv_obj_set_size(fill, fill_w, BAT_H - 4);

    /* 충전 중이면 번개 아이콘 표시 — LVGL v8에서는 lv_obj_add/clear_flag 사용 */
    if (state->charging) {
        lv_obj_clear_flag(bolt_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bolt_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_connection(lv_obj_t *widget, const struct status_state *state) {
    lv_obj_t *conn_label = lv_obj_get_child(widget, OBJ_IDX_CONN_LABEL);

    const char *sym = "";
    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        sym = LV_SYMBOL_USB;
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            sym = state->active_profile_connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
        } else {
            sym = LV_SYMBOL_SETTINGS;
        }
        break;
    }
    lv_label_set_text(conn_label, sym);
}

/* ── 이벤트 콜백 ── */

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    update_battery(widget->obj, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;
    update_connection(widget->obj, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

/* ── 초기화 ── */

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 68, 68);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);

    /* Child 0: flora_00 이미지 (x=-72 → 우측 68px만 보임) */
    lv_obj_t *art = lv_img_create(widget->obj);
    lv_img_set_src(art, &flora_00);
    lv_obj_set_pos(art, -(140 - 68), 0);

    /* Child 1: 배터리 외곽선 */
    lv_obj_t *bat_outline = lv_obj_create(widget->obj);
    lv_obj_set_size(bat_outline, BAT_W, BAT_H);
    lv_obj_set_pos(bat_outline, BAT_X, BAT_Y);
    lv_obj_set_style_bg_opa(bat_outline, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(bat_outline, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bat_outline, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bat_outline, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bat_outline, 1, LV_PART_MAIN);

    /* Child 2: 배터리 채움 바 */
    lv_obj_t *bat_fill = lv_obj_create(widget->obj);
    lv_obj_set_size(bat_fill, (75 * (BAT_W - 4)) / 100, BAT_H - 4);
    lv_obj_set_pos(bat_fill, BAT_X + 2, BAT_Y + 2);
    lv_obj_set_style_bg_color(bat_fill, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bat_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bat_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bat_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bat_fill, 0, LV_PART_MAIN);

    /* Child 3: 배터리 팁 */
    lv_obj_t *bat_tip = lv_obj_create(widget->obj);
    lv_obj_set_size(bat_tip, BAT_TIP_W, BAT_TIP_H);
    lv_obj_set_pos(bat_tip, BAT_X + BAT_W, BAT_Y + (BAT_H - BAT_TIP_H) / 2);
    lv_obj_set_style_bg_color(bat_tip, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bat_tip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bat_tip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bat_tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bat_tip, 1, LV_PART_MAIN);

    /* Child 4: 충전 번개 아이콘 (montserrat_16은 이미 활성화되어 있음) */
    lv_obj_t *bolt_label = lv_label_create(widget->obj);
    lv_label_set_text(bolt_label, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(bolt_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(bolt_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_pos(bolt_label, BAT_X + 6, BAT_Y - 2);
    lv_obj_add_flag(bolt_label, LV_OBJ_FLAG_HIDDEN);  /* 초기에 숨김 */

    /* Child 5: 연결 상태 아이콘 */
    lv_obj_t *conn_label = lv_label_create(widget->obj);
    lv_label_set_text(conn_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(conn_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(conn_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_pos(conn_label, CONN_X, CONN_Y);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_output_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }