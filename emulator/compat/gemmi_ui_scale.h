/* Desktop-only coordinate adapter: preserve the firmware's logical layout,
 * but rasterize text and borders at four times its LCD resolution. */
#ifndef GEMMI_UI_SCALE_H
#define GEMMI_UI_SCALE_H
#define GEMMI_PX(v) ((v) * 4)
static inline void gemmi_measure(lv_point_t *result, const char *text,
                                 const lv_font_t *font, int32_t letter_space,
                                 int32_t line_space, int32_t width, lv_text_flag_t flags)
{
    lv_text_get_size(result, text, font, GEMMI_PX(letter_space),
                     GEMMI_PX(line_space), GEMMI_PX(width), flags);
    result->x = (result->x + 3) / 4;
    result->y = (result->y + 3) / 4;
}
#define lv_text_get_size gemmi_measure
#define lv_obj_set_pos(o,x,y) lv_obj_set_pos(o,GEMMI_PX(x),GEMMI_PX(y))
#define lv_obj_set_size(o,w,h) lv_obj_set_size(o,GEMMI_PX(w),GEMMI_PX(h))
#define lv_obj_set_height(o,h) lv_obj_set_height(o,GEMMI_PX(h))
#define lv_obj_align(o,a,x,y) lv_obj_align(o,a,GEMMI_PX(x),GEMMI_PX(y))
#define lv_image_set_scale(o,s) lv_image_set_scale(o,GEMMI_PX(s))
#define lv_obj_set_style_border_width(o,v,s) lv_obj_set_style_border_width(o,GEMMI_PX(v),s)
#define lv_obj_set_style_pad_all(o,v,s) lv_obj_set_style_pad_all(o,GEMMI_PX(v),s)
#define lv_obj_set_style_pad_hor(o,v,s) lv_obj_set_style_pad_hor(o,GEMMI_PX(v),s)
#define lv_obj_set_style_radius(o,v,s) lv_obj_set_style_radius(o,GEMMI_PX(v),s)
#define lv_obj_set_style_shadow_width(o,v,s) lv_obj_set_style_shadow_width(o,GEMMI_PX(v),s)
#define lv_obj_set_style_text_line_space(o,v,s) lv_obj_set_style_text_line_space(o,GEMMI_PX(v),s)
#endif
