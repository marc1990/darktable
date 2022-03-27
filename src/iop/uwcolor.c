/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/gaussian.h"
#include "common/imagebuf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include "libs/colorpicker.h"
#include "gui/color_picker_proxy.h"


DT_MODULE_INTROSPECTION(10, dt_iop_uwcolor_params_t)

typedef struct dt_iop_uwcolor_params_t
{
  float depth; // $MIN: 0.0 $MAX: 25.0 $DEFAULT: 3.0 $DESCRIPTION: "depth"
  float ch_red; // $MIN: 0.01 $MAX: 1.2 $DEFAULT: 0.85 $DESCRIPTION: "ch_red"
  float ch_green; // $MIN: 0.01 $MAX: 1.2 $DEFAULT: 0.98 $DESCRIPTION: "ch_green"
  float nf_red; // $MIN: -15.00 $MAX: 2.0 $DEFAULT: -8.0 $DESCRIPTION: "nf_red"
  float nf_green; // $MIN: -15.00 $MAX: 2.0 $DEFAULT: -8.0 $DESCRIPTION: "nf_green"
  float noise_sigma; // $MIN: 0.1 $MAX: 3.0 $DEFAULT: 1.0 $DESCRIPTION: "noise_sigma"
  float noise_sigma2; // $MIN: 0.01 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "noise_sigma"
  float levels[3];
} dt_iop_uwcolor_params_t;

typedef struct dt_iop_uwcolor_gui_data_t
{
  GtkWidget *depth;
  GtkWidget *ch_red;
  GtkWidget *ch_green;
  GtkWidget *nf_red;
  GtkWidget *nf_green;
  GtkWidget *noise_sigma;
  GtkWidget *noise_sigma2;
  GtkWidget *bt_select_region;
  
  int draw_selected_region;                     // are we drawing the selected region?
  float posx_from, posx_to, posy_from, posy_to; // coordinates of the area
  int button_down;                              // user pressed the mouse button?

float last_picked_color;
  GtkWidget *blackpick;

} dt_iop_uwcolor_gui_data_t;

typedef struct dt_iop_uwcolor_data_t
{
  float depth;
  float ch_red;
  float ch_green;
  float nf_red;
  float nf_green;
  float noise_sigma;
  float noise_sigma2;
} dt_iop_uwcolor_data_t;


const char *name()
{
  return _("uwcolor");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING ;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

int input_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                     dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

int output_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("saturate and reduce the lightness of the most saturated pixels\n"
                                        "to make the colors more vivid."),
                                      _("creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

static void _turn_select_region_off(struct dt_iop_module_t *self)
{
  dt_iop_uwcolor_gui_data_t *c = (dt_iop_uwcolor_gui_data_t *)self->gui_data;
  if(c)
  {
    c->button_down = c->draw_selected_region = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->bt_select_region), c->draw_selected_region);
  }
}
/*
static void _turn_selregion_picker_off(struct dt_iop_module_t *self)
{
  _turn_select_region_off(self);
  dt_iop_color_picker_reset(self, TRUE);
}
*/

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  fprintf(stderr, "channels: %d\n", piece->pipe->dsc.channels);
  fprintf(stderr, "filters: 0x%08x\n", piece->pipe->dsc.filters);
  fprintf(stderr, "temperature.enabled: 0x%08x\n", piece->pipe->dsc.temperature.enabled);
  fprintf(stderr, "cst: 0x%08x\n", piece->pipe->dsc.cst);
  if (!dt_iop_have_required_input_format(1 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const dt_iop_uwcolor_data_t *const d = (dt_iop_uwcolor_data_t *)piece->data;
  const uint32_t filters = piece->pipe->dsc.filters;

  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;
  float * low_res;
  float * low_res_gaus;
  float * low_res_gaus2;

  const float sigma = d->noise_sigma * roi_in->scale / piece->iscale;
  const float sigma2 = d->noise_sigma2 * roi_in->scale / piece->iscale;

  uint32_t width_small =  roi_out->width/2;
  uint32_t height_small =  roi_out->height/2;

  // GBRG
  int pos_green0[2] = {0,0};
  int pos_blue[2] = {0,1};
  int pos_red[2] = {1,0};
  int pos_green1[2] = {1,1};

  if(filters == 0x94949494){ //rggb
    pos_green0[0] = 0;
    pos_green0[1] = 1;
    pos_blue[0] = 1;
    pos_blue[1] = 1;
    pos_red[0] = 0;
    pos_red[1] = 0;
    pos_green1[0] = 1;
    pos_green1[1] = 0;
  } else if(filters == 0x9) { // xtrans
     dt_iop_set_module_trouble_message(self, _("unsupported xtrans input"),
                                        _(" Only bayer filter rggb and gbrg are supported"),
                                        "unsupported data format at current pipeline position");
    return;
  } else if(filters != 0x49494949) { // also not gbrg
     dt_iop_set_module_trouble_message(self, _("unsupported bayer format at input"),
                                        _(" Only bayer filter rggb and gbrg are supported"),
                                        "unsupported data format at current pipeline position");
    return;
  }
  
  low_res = dt_iop_image_alloc(width_small,height_small,4);
  dt_iop_image_fill(low_res,0.0f,width_small,height_small,4);

  low_res_gaus = dt_iop_image_alloc(width_small,height_small,4);
  low_res_gaus2 = dt_iop_image_alloc(width_small,height_small,4);

  fprintf(stderr, "Has alloc all low res image buffers\n");
  
  // copy pixel from bayer to single channel
  if(filters)
  { // bayer float mosaiced
    for(int x = pos_red[0]; x < height_small*2; x += 2) // row
    {
      for(int y = pos_red[1]; y < (width_small*2); y += 2) // col
      {
        low_res[(x/2*(width_small)+y/2)*4+0] = in[x*roi_out->width+y];
      }
    }
    for(int x = pos_green0[0]; x < height_small*2; x += 2) // row
    {
      for(int y = pos_green0[1]; y < (width_small*2); y += 2) // col
      {
        low_res[(x/2*(width_small)+y/2)*4+1] += in[x*roi_out->width+y]/2;
      }
    }
    for(int x = pos_green1[0]; x < height_small*2; x += 2) // row
    {
      for(int y = pos_green1[1]; y < width_small*2; y += 2) // col
      {
        low_res[(x/2*(width_small)+y/2)*4+1]  += in[x*roi_out->width+y]/2;
      }
    }
    for(int x = pos_blue[0]; x < height_small*2; x += 2) // row
    {
      for(int y = pos_blue[0]; y < width_small*2; y += 2) // col
      {
        low_res[(x/2*(width_small)+y/2)*4+2]  = in[x*roi_out->width+y];
      }
    }
  }else{
    // non-mosaiced (will never happen because of input checks)
    fprintf(stderr, "non-mosaiced\n");
  }

  fprintf(stderr, "finished copy pixels\n");

  // Create a gaussian blurred version, use to restore color at low dynamic range
  const float clipmax[4] = {100.0,100.0,100.0,100.0};
  const float clipmin[4] = {0.0,0.0,0.0,0.0};
  dt_gaussian_t *g = dt_gaussian_init(
    width_small, 
    height_small, 
    4, 
    clipmax, clipmin, sigma, DT_IOP_GAUSSIAN_ZERO);
  if(!g) return;

  dt_gaussian_t *g2 = dt_gaussian_init(
    width_small, 
    height_small, 
    4, 
    clipmax, clipmin, sigma2, DT_IOP_GAUSSIAN_ZERO);
  if(!g) return;

  dt_gaussian_blur_4c(g,low_res,low_res_gaus);
  dt_gaussian_blur_4c(g2,low_res,low_res_gaus2);
  dt_gaussian_free(g);

  dt_free_align(low_res);

  fprintf(stderr, "Done blur\n");

  const float depth = (d->depth);
  const float red = (d->ch_red);
  const float green = (d->ch_green);
  const float nf_red = (d->nf_red);
  const float nf_green = (d->nf_green);
  //const int npixels = roi_out->height * roi_out->width;

  const float lr = pow(red,depth); // loss red
  const float lg = pow(green,depth); // loss green
  const float gr = 1/lr; // compensation gain red
  const float gg = 1/lg; // compensation gain greem
  const float gb = 1; // compensation gain blue, aka none

  const float power_noise_factor = 2.0;

  for(int x = 0; x < roi_out->height; x += 1) // row
  {
    for(int y = 0; y < roi_out->width; y += 1) // col
    {
      const uint32_t small_x = (x >= (height_small*2)) ? (height_small-1) : (x/2);
      const uint32_t small_y = (y >= (width_small*2)) ? (width_small-1) : (y/2);
      
      // gaussian color corrected "brightness" (ya)
      const float ya = low_res_gaus[(small_x*(width_small)+small_y)*4+0]*gr + 
      low_res_gaus[(small_x*(width_small)+small_y)*4+1]*gg +
      low_res_gaus[(small_x*(width_small)+small_y)*4+2]*gb; 
      

      
      // GBRG
      // calculate local color corrected "brightness" (yl)
      const float yl = low_res_gaus2[(small_x*(width_small)+small_y)*4+0]*gr + 
      low_res_gaus2[(small_x*(width_small)+small_y)*4+1]*gg + 
      low_res_gaus2[(small_x*(width_small)+small_y)*4+2]*gb;
  

      // Calculate to local gain compare to gaussian
      const float gain = yl/ya;

      // When channel value are close/lower to noise floor use the gaussian value (only for red and green)
      // First caclulete the exposure of pixel in ev (evg/evr)
      // Next substract the nf_green/red and use power function
      // with power_noise_factor to smooth out transition to gaussian.

      if(x % 2 == pos_green0[0] && y % 2 == pos_green0[1]){ // green0
        const float evg = log2f(low_res_gaus[(small_x*(width_small)+small_y)*4+1]);
        const float nd = pow(power_noise_factor,fmin(evg - nf_green,0.0));        
        out[x*roi_out->width+y] = (in[x*roi_out->width+y]*gg*nd + 
        low_res_gaus[(small_x*(width_small)+small_y)*4+1]*gg*(1-nd)*gain);

      }else if(x % 2 == pos_blue[0] && y % 2 == pos_blue[1]){ // blue
        out[x*roi_out->width+y] = in[x*roi_out->width+y]*gb;

      }else if(x % 2 == pos_red[0] && y % 2 == pos_red[1]){ // red
        const float evr = log2f(low_res_gaus[(small_x*(width_small)+small_y)*4+0]);
        const float nd = pow(power_noise_factor,fmin(evr- nf_red,0.0));
        out[x*roi_out->width+y] = (in[x*roi_out->width+y]*gr*nd+ 
        low_res_gaus[(small_x*(width_small)+small_y)*4+0]*gr*(1-nd)*gain); 

      }else{ // green1
        const float evg = log2f(low_res_gaus[(small_x*(width_small)+small_y)*4+1]);
        const float nd = pow(power_noise_factor,fmin(evg - nf_green,0.0));
        out[x*roi_out->width+y] = (in[x*roi_out->width+y]*gg*nd+ 
        low_res_gaus[(small_x*(width_small)+small_y)*4+1]*gg*(1-nd)*gain);
      }
    }
  }

  fprintf(stderr, "Done filter\n");
  dt_free_align(low_res_gaus);
  dt_free_align(low_res_gaus2);  
}



static void _color_picker_callback(GtkWidget *button, dt_iop_module_t *self)
{
  _turn_select_region_off(self);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_uwcolor_gui_data_t *g = (dt_iop_uwcolor_gui_data_t *)self->gui_data;
  dt_iop_uwcolor_params_t *p = (dt_iop_uwcolor_params_t *)self->params;

  const dt_iop_uwcolor_data_t *const d = (dt_iop_uwcolor_data_t *)piece->data;

  



  /* we need to save the last picked color to prevent flickering when
   * changing from one picker to another, as the picked_color value does not
   * update as rapidly */
  const float mean_picked_color = *self->picked_color;

  if(mean_picked_color != g->last_picked_color)
  {
    dt_aligned_pixel_t previous_color;
    previous_color[0] = p->levels[0];
    previous_color[1] = p->levels[1];
    previous_color[2] = p->levels[2];

   

    g->last_picked_color = mean_picked_color;

    
      if(mean_picked_color > p->levels[1])
      {
        p->levels[0] = p->levels[1] - FLT_EPSILON;
      }
      else
      {
        p->levels[0] = mean_picked_color;
      }
      fprintf(stderr, "Levels in %f %f %f\n", self->picked_color[0], self->picked_color[1],self->picked_color[2]);
    // 0,040740 0,201517 0,210226
    // goal 0,210226 0,210226 0,210226
    // 0,193791443
 const float new_red = pow(self->picked_color[0]/self->picked_color[2],1/d->depth); // pow(0.04176/0.2026,1/3)
    const float  new_green = pow(self->picked_color[1]/self->picked_color[2],1/d->depth); // pow(0.1991/0.2026,1/3)
fprintf(stderr, "new in %f %f\n", new_red, new_green);

    dt_bauhaus_slider_set(g->ch_red,  new_red);
    dt_bauhaus_slider_set(g->ch_green, new_green);
    
    if(previous_color[0] != p->levels[0]
       || previous_color[1] != p->levels[1]
       || previous_color[2] != p->levels[2])
    {
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }

}

static void _select_region_toggled_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_uwcolor_gui_data_t *g = (dt_iop_uwcolor_gui_data_t *)self->gui_data;

  dt_iop_request_focus(self);
  if(self->off)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  dt_iop_color_picker_reset(self, TRUE);

  dt_iop_gui_enter_critical_section(self);

  if(gtk_toggle_button_get_active(togglebutton))
  {
    g->draw_selected_region = 1;
  }
  else
    g->draw_selected_region = 0;

  g->posx_from = g->posx_to = g->posy_from = g->posy_to = 0;

  dt_iop_gui_leave_critical_section(self);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_uwcolor_params_t *p = (dt_iop_uwcolor_params_t *)p1;
  dt_iop_uwcolor_data_t *d = (dt_iop_uwcolor_data_t *)piece->data;
  d->depth = p->depth;
  d->ch_red = p->ch_red;
  d->ch_green = p->ch_green;
  d->nf_red = p->nf_red;
  d->nf_green = p->nf_green;
  d->noise_sigma = p->noise_sigma;
  d->noise_sigma2 = p->noise_sigma2;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_uwcolor_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_uwcolor_gui_data_t *g = (dt_iop_uwcolor_gui_data_t *)self->gui_data;
  dt_iop_uwcolor_params_t *p = (dt_iop_uwcolor_params_t *)self->params;
  dt_bauhaus_slider_set(g->depth, p->depth);
  dt_bauhaus_slider_set(g->ch_red, p->ch_red);
  dt_bauhaus_slider_set(g->ch_green, p->ch_green);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_uwcolor_gui_data_t *g = IOP_GUI_ALLOC(uwcolor);

  g->depth = dt_bauhaus_slider_from_params(self, "depth");
  dt_bauhaus_slider_set_format(g->depth, "%.2f");
  gtk_widget_set_tooltip_text(g->depth, _("the length of light path in water"));

  g->ch_red = dt_bauhaus_slider_from_params(self, "ch_red");
  dt_bauhaus_slider_set_format(g->ch_red, "%.3f");
  gtk_widget_set_tooltip_text(g->ch_red, _("red absoption weight normilize to 1 m"));

  g->ch_green = dt_bauhaus_slider_from_params(self, "ch_green");
  dt_bauhaus_slider_set_format(g->ch_green, "%.3f");
  gtk_widget_set_tooltip_text(g->ch_green, _("green absoption weight normilize to 1 m"));

  g->nf_red = dt_bauhaus_slider_from_params(self, "nf_red");
  dt_bauhaus_slider_set_format(g->nf_red, "%.2f");
  gtk_widget_set_tooltip_text(g->nf_red, _("noise floor in EV for red"));

  g->nf_green = dt_bauhaus_slider_from_params(self, "nf_green");
  dt_bauhaus_slider_set_format(g->nf_green, "%.2f");
  gtk_widget_set_tooltip_text(g->nf_green, _("noise floor in EV for green"));

  g->noise_sigma = dt_bauhaus_slider_from_params(self, "noise_sigma");
  dt_bauhaus_slider_set_format(g->noise_sigma, "%.2f");
  gtk_widget_set_tooltip_text(g->noise_sigma, _("Gaussian sigma used for color reconstruct"));

  g->noise_sigma2 = dt_bauhaus_slider_from_params(self, "noise_sigma2");
  dt_bauhaus_slider_set_format(g->noise_sigma2, "%.3f");
  gtk_widget_set_tooltip_text(g->noise_sigma2, _("Gaussian sigma used for color reconstruct"));

  g->blackpick = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  dt_action_define_iop(self, "pickers", "black", g->blackpick, &dt_action_def_toggle);
  gtk_widget_set_tooltip_text(g->blackpick, _("pick black point from image"));
  gtk_widget_set_name(GTK_WIDGET(g->blackpick), "picker-black");
  g_signal_connect(G_OBJECT(g->blackpick), "toggled", G_CALLBACK(_color_picker_callback), self);

  GtkWidget *pick_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(pick_hbox), GTK_WIDGET(g->blackpick), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), pick_hbox, TRUE, TRUE, 0);

  g->bt_select_region = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT, NULL);
  dt_action_define_iop(self, NULL, "auto region", g->bt_select_region, &dt_action_def_toggle);
  gtk_widget_set_tooltip_text(g->bt_select_region,
                              _("apply auto levels based on a region defined by the user\n"
                                "click and drag to draw the area\n"
                                "right click to cancel"));
  
  g_signal_connect(G_OBJECT(g->bt_select_region), "toggled", G_CALLBACK(_select_region_toggled_callback), self);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
