/*
   The Resynthesizer - A GIMP plug-in for resynthesizing textures
   Copyright (C) 2000 2008  Paul Francis Harrison
   Copyright (C) 2002  Laurent Despeyroux
   Copyright (C) 2002  David Rodríguez García

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <gtk/gtk.h>

#include <vector>
#include <algorithm>

using namespace std;

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

/* Inclusion : Laurent Despeyroux
   This is for other GNU distributions with internationalized messages.
   When compiling libc, the "_" macro is predefined.  */
#ifndef _
#include <libintl.h>
#define _(msgid)    gettext (msgid)
#endif

typedef guint8 Pixelel;

const int max_neighbours = 1000,
      max_trys_per_pixel = 10000;

/***********************/
/***********************/
/*** x,y Coordinates ***/
/***********************/
/***********************/

struct Coordinates {
    int x, y;
    Coordinates(int _x,int _y): x(_x), y(_y) { }
    Coordinates() { }

    bool operator < (const Coordinates other) const { 
        return y*y+x*x < (other.y*other.y+other.x*other.x); 
    }

    Coordinates operator + (const Coordinates a) const { 
        return Coordinates(x+a.x,y+a.y); 
    }

    Coordinates operator - (const Coordinates a) const { 
        return Coordinates(x-a.x,y-a.y); 
    }
};


/**************/
/**************/
/*** Bitmap ***/
/**************/
/**************/

//Bitmap class with three dimensions (width, height, number of channels)
template<class t>
struct Bitmap {
    int width, height, depth;
    t *data;

    Bitmap() { 
        data = 0;
    }

    ~Bitmap() {
        delete data;
    }

    void size(int w,int h,int d) {
        width = w; 
        height = h; 
        depth = d;

        delete data;
        data = new t[w*h*d];
    }

    t *at(int x,int y) const {
        return data + (y*width+x)*depth;
    }

    t *at(const Coordinates position) const {
        return at(position.x,position.y);
    }


    void to_drawable(GimpDrawable *drawable, int x1,int y1, int src_layer) {
        int i,j;
        GimpPixelRgn region;
        guchar *img;

        gimp_pixel_rgn_init(&region, drawable, x1,y1,width,height, TRUE,TRUE);

        img = new guchar[width*height*depth];

        for(i=0;i<width*height;i++)
            for(j=0;j<int(drawable->bpp);j++)
                img[i*drawable->bpp+j] = data[i*depth+src_layer+j];

        gimp_pixel_rgn_set_rect(&region, img, x1,y1,width,height);

        delete img;
    }

    void from_drawable(GimpDrawable *drawable, 
            int x1,int y1, int dest_layer) {
        int i,j;
        GimpPixelRgn region;
        guchar *img;

        gimp_pixel_rgn_init(&region, drawable, x1,y1,width,height, FALSE,FALSE);

        img = new guchar[width*height*depth];
        gimp_pixel_rgn_get_rect(&region, img, x1,y1,width,height);

        for(i=0;i<width*height;i++)
            for(j=0;j<(int)(drawable->bpp);j++)
                data[i*depth+dest_layer+j] = img[i*drawable->bpp+j];

        delete img;
    }
};

/*******************************/
/*******************************/
/*** Status holding type *******/
/*******************************/
/*******************************/

struct Status {
    bool has_value, has_source;
    Coordinates source;
};

/*******************************/
/*******************************/
/*** Get stuff from GIMP *******/
/*******************************/
/*******************************/

//Get a drawable and possibly its selection mask from the GIMP
static void fetch_image_and_mask(GimpDrawable *drawable, Bitmap<Pixelel> &image, int bytes, 
        Bitmap<Pixelel> &mask, Pixelel default_mask_value)
{
    int x,y,x1,y1,x2,y2;
    Bitmap<Pixelel> temp_mask;
    int sel_id;
    int xoff,yoff;
    int has_selection;
    GimpDrawable *mask_drawable;

    image.size(drawable->width, drawable->height, bytes);
    mask.size(drawable->width, drawable->height, 1);

    image.from_drawable(drawable,0,0,0);

    has_selection = gimp_drawable_mask_bounds(drawable->drawable_id,&x1,&y1,&x2,&y2);

    if (!has_selection) {
        memset(mask.data, default_mask_value, mask.width*mask.height*sizeof(Pixelel));
        return;
    }

    temp_mask.size(x2-x1, y2-y1, 1);

    sel_id = gimp_image_get_selection(gimp_drawable_get_image(drawable->drawable_id));
    mask_drawable = gimp_drawable_get(sel_id);

    gimp_drawable_offsets(drawable->drawable_id, &xoff, &yoff);

    temp_mask.from_drawable(mask_drawable, x1+xoff,y1+yoff, 0);

    gimp_drawable_detach(mask_drawable);

    memset(mask.data, 0, mask.width*mask.height*sizeof(Pixelel));
    for(y=0;y<temp_mask.height;y++)
        for(x=0;x<temp_mask.width;x++)
            mask.at(x+x1,y+y1)[0] = temp_mask.at(x,y)[0];
}


/******************/
/******************/
/*** Parameters ***/
/******************/
/******************/

struct Parameters {
    bool h_tile, v_tile;
    bool use_border;

    gint32 corpus_id, input_map_id, output_map_id;

    double map_weight;
    double autism;
    gint32 neighbours, trys;
};


static inline bool wrap_or_clip(Parameters &parameters, Bitmap<Pixelel> &image, Coordinates &point) { 
    while(point.x < 0)
        if (parameters.h_tile)
            point.x += image.width;
        else
            return false;

    while(point.x >= image.width)
        if (parameters.h_tile)
            point.x -= image.width;
        else
            return false;

    while(point.y < 0)
        if (parameters.v_tile)
            point.y += image.height;
        else
            return false;

    while(point.y >= image.height)
        if (parameters.v_tile)
            point.y -= image.height;
        else
            return false;

    return true;
}

/*****************/
/*****************/
/*** Interface ***/
/*****************/
/*****************/

    static void click_func_quit(GtkWidget *widget, gpointer data) {
        if (data)
            *((bool*)data) = true;
        gtk_main_quit();
    }

    static gint delete_func_quit(GtkWidget *widget, GdkEvent *event, gpointer data) {
        if (data)
            *((bool*)data) = true;
        gtk_main_quit();
        return TRUE;
    }

static gint desired_corpus_bpp;
static gint corpus_menu_constrain(gint32 image_id, gint32 drawable_id, gpointer data) {
    return
        drawable_id == -1 || (
                (gimp_drawable_is_rgb(drawable_id) || gimp_drawable_is_gray(drawable_id))
                && gimp_drawable_bpp(drawable_id) == desired_corpus_bpp);
}

static gint map_menu_constrain(gint32 image_id, gint32 drawable_id, gpointer data) {
    return
        drawable_id == -1 || 
        gimp_drawable_is_rgb(drawable_id) || 
        gimp_drawable_is_gray(drawable_id);
}

static void menu_callback(gint32 id, gpointer data) {
    *((gint32*)data) = id;
}

static void checkbutton_callback(GtkWidget *widget, gpointer data) {
    *((bool*)data) = GTK_TOGGLE_BUTTON(widget)->active;
}

static GtkWidget *map_input, *map_output, *map_slider;

static void map_checkbutton_callback(GtkWidget *widget, gpointer data) {
    *((bool*)data) = GTK_TOGGLE_BUTTON(widget)->active;
    gtk_widget_set_sensitive(map_input,  *((bool*)data));
    gtk_widget_set_sensitive(map_output, *((bool*)data));
    gtk_widget_set_sensitive(map_slider, *((bool*)data));
}

static GtkWidget *make_image_menu(char *text, GimpConstraintFunc constraint,
        gint32 *value) {
    GtkWidget *box, *label, *menu, *option_menu;

    box = gtk_hbox_new(FALSE,0);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);

    label = gtk_label_new(text);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE,FALSE,0);

    menu = gimp_drawable_menu_new(constraint,
            menu_callback, value, *value);

    option_menu = gtk_option_menu_new();
    gtk_option_menu_set_menu(GTK_OPTION_MENU(option_menu), menu);
    gtk_box_pack_end(GTK_BOX(box), option_menu, FALSE,FALSE,0);

    return box;
}

static GtkWidget *make_checkbutton(char *label, bool *state) {
    GtkWidget *checkbutton = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton), *state);
    gtk_signal_connect(GTK_OBJECT(checkbutton), "toggled",
            GTK_SIGNAL_FUNC(checkbutton_callback), state);

    return checkbutton;
}

static GtkWidget *make_slider_double(char *label, double *variable) {
    GtkObject *adjustment;
    GtkWidget *table;

    table = gtk_table_new(1,3,FALSE);
    gtk_table_set_col_spacings(GTK_TABLE (table), 4);
    gtk_container_border_width(GTK_CONTAINER (table), 4);

    adjustment = 
        gimp_scale_entry_new(GTK_TABLE(table), 0,0, label, 125,75, 
                *variable, 0.01,1.0,0.01,1.0,2,
                TRUE,0.0,1.0, NULL,NULL);

    gtk_signal_connect (GTK_OBJECT(adjustment), "value_changed",
            GTK_SIGNAL_FUNC(gimp_double_adjustment_update),
            variable);

    return table;                   
}

static GtkWidget *make_slider_int(char *label, gint32 *variable, int upper_bound, int real_upper_bound) {
    GtkObject *adjustment;
    GtkWidget *table;

    table = gtk_table_new(1,3,FALSE);
    gtk_table_set_col_spacings(GTK_TABLE (table), 4);
    gtk_container_border_width(GTK_CONTAINER (table), 4);

    adjustment = 
        gimp_scale_entry_new(GTK_TABLE(table), 0,0, label, 125,75, 
                *variable, 1,upper_bound,1.0,1.0,0,
                FALSE,1,real_upper_bound, NULL,NULL);

    gtk_signal_connect (GTK_OBJECT(adjustment), "value_changed",
            GTK_SIGNAL_FUNC(gimp_int_adjustment_update),
            variable);

    return table;                   
}


static GtkWidget *make_multiline_label(char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0,0.0);
    return label;
}

/* Open a dialog, get user to select options */
static bool get_parameters_by_asking(Parameters *param, int default_drawable) {
    GtkWidget *window, 
              *button_box, *ok, *cancel,
              *notebook, *main_box, *tweaks_box,
              *input_frame, *corpus, 
              *output_frame, *output_box, *h_tile_button, *v_tile_button, *border_button,
              *map_frame, *map_box, *map_button,
              *personality_slider, *neighbours_slider, *trys_slider;
    bool map;
    bool cancelled = false;

    window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(window), _("Resynthesize"));
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
    gtk_signal_connect(GTK_OBJECT(window), "delete_event",
            GTK_SIGNAL_FUNC(delete_func_quit), &cancelled);

    /* Buttons */

    button_box = gtk_hbutton_box_new();
    gtk_button_box_set_spacing(GTK_BUTTON_BOX(button_box), 4);
    gtk_box_pack_end(GTK_BOX(GTK_DIALOG(window)->action_area),button_box, 
            FALSE,FALSE,0);

    ok = gtk_button_new_with_label(_("OK"));
    GTK_WIDGET_SET_FLAGS(ok, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(ok), "clicked",
            GTK_SIGNAL_FUNC(click_func_quit), 0);
    gtk_box_pack_start(GTK_BOX(button_box), ok, FALSE,FALSE,0);
    gtk_widget_grab_default(ok);

    cancel = gtk_button_new_with_label(_("Cancel"));
    GTK_WIDGET_SET_FLAGS(cancel, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(cancel), "clicked",
            GTK_SIGNAL_FUNC(click_func_quit), &cancelled);
    gtk_box_pack_start(GTK_BOX(button_box), cancel, FALSE,FALSE,0);

    /* Notebook */

    notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(window)->vbox), notebook, FALSE,FALSE,0);

    main_box = gtk_vbox_new(FALSE,0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), main_box,
            gtk_label_new(_("Options")));

    tweaks_box = gtk_vbox_new(FALSE,0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tweaks_box,
            gtk_label_new(_("Tweaks")));

    /* Main page */

    input_frame = gtk_frame_new(_("Input"));
    gtk_frame_set_shadow_type(GTK_FRAME(input_frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_set_border_width(GTK_CONTAINER(input_frame), 6);
    gtk_box_pack_start(GTK_BOX(main_box), input_frame, TRUE, TRUE, 0);

    desired_corpus_bpp = gimp_drawable_bpp(default_drawable);
    corpus = make_image_menu(_("Texture source: "), corpus_menu_constrain,
            &param->corpus_id);
    gtk_container_add(GTK_CONTAINER(input_frame), corpus);

    output_frame = gtk_frame_new(_("Output"));
    gtk_frame_set_shadow_type(GTK_FRAME(output_frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_set_border_width(GTK_CONTAINER(output_frame), 6);
    gtk_box_pack_start(GTK_BOX(main_box), output_frame, TRUE, TRUE, 0);

    output_box = gtk_vbox_new(FALSE,0);
    gtk_container_add(GTK_CONTAINER(output_frame), output_box);

    h_tile_button = make_checkbutton(_("Make horizontally tileable"),
            &param->h_tile);
    gtk_box_pack_start_defaults(GTK_BOX(output_box), h_tile_button);

    v_tile_button = make_checkbutton(_("Make vertically tileable"),
            &param->v_tile);
    gtk_box_pack_start_defaults(GTK_BOX(output_box), v_tile_button);

    border_button = make_checkbutton(_("Fit output to bordering pixels"),
            &param->use_border);
    gtk_box_pack_start_defaults(GTK_BOX(output_box), border_button);

    map_frame = gtk_frame_new(_("Texture transfer"));
    gtk_frame_set_shadow_type(GTK_FRAME(map_frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_set_border_width(GTK_CONTAINER(map_frame), 6);
    gtk_box_pack_start(GTK_BOX(main_box), map_frame, TRUE, TRUE, 0);

    map_box = gtk_vbox_new(FALSE,0);
    gtk_container_add(GTK_CONTAINER(map_frame), map_box);

    map = (param->input_map_id != -1 && param->output_map_id != -1);
    map_button = gtk_check_button_new_with_label(_("Use texture transfer"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(map_button), map);
    gtk_signal_connect(GTK_OBJECT(map_button), "toggled",
            GTK_SIGNAL_FUNC(map_checkbutton_callback), &map);
    gtk_box_pack_start_defaults(GTK_BOX(map_box), map_button);

    map_input = make_image_menu(_("Input map: "), map_menu_constrain,
            &param->input_map_id);
    gtk_widget_set_sensitive(map_input, map);
    gtk_box_pack_start_defaults(GTK_BOX(map_box), map_input);

    map_output = make_image_menu(_("Output map: "), map_menu_constrain,
            &param->output_map_id);
    gtk_widget_set_sensitive(map_output, map);
    gtk_box_pack_start_defaults(GTK_BOX(map_box), map_output);

    map_slider = make_slider_double(_("Map importance:"),
            &param->map_weight);
    gtk_widget_set_sensitive(map_slider, map);
    gtk_box_pack_start_defaults(GTK_BOX(map_box), map_slider);

    /* Tweaks page */

    gtk_box_pack_start(GTK_BOX(tweaks_box), make_multiline_label(_("Resynthesizer operates by copying pixels, one at a time, from the input to the output. A neighbourhood of nearby pixels in the output are compared with neighbourhoods of pixels in the input. The pixel at the center of the best matching neighbourhood is copied to the output.\n")), FALSE,FALSE,4);

    neighbours_slider = make_slider_int(_("Neighbourhood size:"), &param->neighbours, 100, max_neighbours);
    gtk_box_pack_start(GTK_BOX(tweaks_box), neighbours_slider, FALSE,FALSE,0);

    gtk_box_pack_start(GTK_BOX(tweaks_box), make_multiline_label(_("- How many nearby pixels in the output are to be used?\n")), FALSE,FALSE,0); 

    trys_slider = make_slider_int(_("Search thoroughness:"), &param->trys, 500, max_trys_per_pixel);
    gtk_box_pack_start(GTK_BOX(tweaks_box), trys_slider, FALSE,FALSE,0);

    gtk_box_pack_start(GTK_BOX(tweaks_box), make_multiline_label(_("- How many locations in the input are examined?\n")), FALSE,FALSE,0); 

    personality_slider = make_slider_double(_("Sensitivity to outliers:"), &param->autism);
    gtk_box_pack_start(GTK_BOX(tweaks_box), personality_slider, FALSE,FALSE,0);

    gtk_box_pack_start(GTK_BOX(tweaks_box), make_multiline_label(_("- To what extent does a very bad match on a single pixel disqualify a location?\n")), FALSE,FALSE,0); 

    gtk_widget_show_all(window);

    gtk_main();

    gtk_widget_destroy(window);

    gdk_flush();

    if (!map) {
        param->input_map_id = -1;
        param->output_map_id = -1;
    }

    if (cancelled)
        return false;

    return true;
}

/* Restore the last parameters used from deep in the bowels of
   the main GIMP app */
static bool get_last_parameters(Parameters *param, int default_drawable) {
    /* Defaults in case this is our first run */
    param->corpus_id = -1;
    param->input_map_id = -1;
    param->output_map_id = -1;
    param->v_tile = true;
    param->h_tile = true;
    param->use_border = true;
    param->map_weight = 0.5;
    param->autism = 0.117; /* 30/256 */
    param->neighbours = 30;
    param->trys = 200;

    gimp_get_data("plug_in_resynthesizer", param);

    /* If image was resynthesized from itself last time, resythesize this 
       image from itself too */
    if (param->corpus_id == -1)
        param->corpus_id = default_drawable;

    return true;
}

/* Convert argument list into parameters */
    static bool get_parameters_from_list(Parameters *param, int n_args, const GimpParam *args) {
        if (n_args != 13)
            return false;

        param->v_tile        = args[3].data.d_int32;
        param->h_tile        = args[4].data.d_int32;
        param->use_border    = args[5].data.d_int32;
        param->corpus_id     = args[6].data.d_int32;
        param->input_map_id  = args[7].data.d_int32;
        param->output_map_id = args[8].data.d_int32;
        param->map_weight    = args[9].data.d_float;
        param->autism        = args[10].data.d_float;
        param->neighbours    = args[11].data.d_int32;
        param->trys          = args[12].data.d_int32;

        return true;
    }

/* Talking to GIMP stuff */

static void query();
static void run(const gchar *name,
        gint nparams,
        const GimpParam *param,
        gint *nreturn_vals,
        GimpParam **return_vals);

static GimpPlugInInfo PLUG_IN_INFO = {
    NULL, /* init_proc */
    NULL, /* quit_proc */
    query, /* query_proc */
    run, /* run_proc */
};

/* Macro to define the usual plugin main function */
MAIN()

    /* Add capabilities of this plugin to Procedural DataBase */
    static void query() {
        static GimpParamDef args[] = {
            { GIMP_PDB_INT32, "run_mode", "Interactive, non-interactive" },
            { GIMP_PDB_IMAGE, "image", "Input image" },
            { GIMP_PDB_DRAWABLE, "drawable", "Input drawable" },

            { GIMP_PDB_INT32, "vtile", "Make tilable vertically" },
            { GIMP_PDB_INT32, "htile", "Make tilable horizontally" },
            { GIMP_PDB_INT32, "noborder", "Dont change border pixels" },
            { GIMP_PDB_INT32, "corpus", "Layer to use as corpus" },
            { GIMP_PDB_INT32, "inmask", "Layer to use as input mask, -1 for none" },
            { GIMP_PDB_INT32, "outmask", "Layer to use as output mask, -1 for none" },
            { GIMP_PDB_FLOAT, "map_weight", "Weight to give to map, if map is used" },
            { GIMP_PDB_FLOAT, "autism", "Sensitivity to outliers" },
            { GIMP_PDB_INT32, "neighbourhood", "Neighbourhood size" },
            { GIMP_PDB_INT32, "trys", "Search thoroughness" },
        };

        GimpParamDef *return_vals = NULL;
        gint nargs = sizeof(args)/sizeof(args[0]);
        gint nreturn_vals = 0;

        gimp_install_procedure("plug-in-resynthesizer",
                "Make tiles,"
                "apply themes to images, "
                "remove unwanted features, etc.",
                "",
                "Paul Francis Harrison",
                "Paul Francis Harrison",
                "2000",
                "<Image>/Filters/Map/Resynthesize...",
                "RGB*, GRAY*",
                GIMP_PLUGIN,
                nargs, nreturn_vals,
                args, return_vals);
    }

static void init_gtk() {
    int argc = 1;
    char **argv = g_new(char*,2);
    argv[0] = "resynthesizer";
    argv[1] = 0;
    gtk_init(&argc, &argv);
    gtk_rc_parse(gimp_gtkrc());
}

/* Helpers for the main function */

static int diff_table[512], map_diff_table[512];

static int input_bytes, map_bytes, map_pos, bytes;

// we must fill data_mask subset of data
// using corpus_mask subset of corpus for inspiration
// status holds current state of point filling

static Bitmap<Pixelel> data, data_mask, corpus, corpus_mask;
static Bitmap<Status> status;

// data_points is a queue of points to be filled,
// it can contain duplicates, which mean points re-analysis
static vector<Coordinates> data_points, corpus_points, sorted_offsets;

static vector<Coordinates> neighbour_offsets(0);
static Pixelel neighbour_values[max_neighbours][8];
static vector<Status*> neighbour_statuses(0);
static int n_neighbours;

static int best;
static Coordinates best_point;

static double neglog_cauchy(double x) {
    return log(x*x+1.0);
}

static void make_offset_list(void) {
    int width = (corpus.width<data.width?corpus.width:data.width);
    int height = (corpus.height<data.height?corpus.height:data.height);
    sorted_offsets.resize(0);
    for(int y=-height+1;y<height;y++)
        for(int x=-width+1;x<width;x++)
            sorted_offsets.push_back(Coordinates(x,y));

    sort(sorted_offsets.begin(), sorted_offsets.end());
}

static int get_metric(const Coordinates& point, int best_so_far)
{
    int sum = 0;
    for(int i=neighbour_offsets.size()-1; i>=0; --i) {
        Coordinates off_point = point + neighbour_offsets[i];
        if (off_point.x < 0 || off_point.y < 0 || 
                off_point.x >= corpus.width || off_point.y >= corpus.height ||
                !*corpus_mask.at(off_point)) {
            sum += diff_table[0]*input_bytes + map_diff_table[0]*map_bytes;
        } else {
            const Pixelel *corpus_pixel = corpus.at(off_point),
                  *data_pixel = neighbour_values[i];
            if (i)
                for(int j=0;j<input_bytes;j++)
                    sum += diff_table[256u + data_pixel[j] - corpus_pixel[j]];
            if (input_bytes != bytes)
                for(int j=input_bytes;j<bytes;j++)
                    sum += map_diff_table[256u + data_pixel[j] - corpus_pixel[j]];
        }

        if (sum >= best_so_far)
            return sum;
    }
    return sum;
}

/*
// L1 (SAD) metric
static int get_metric(const Coordinates& point, int best_so_far)
{
    int sum = 0;
    for(int i=0; i<n_neighbours; ++i) {
        Coordinates off_point = point + neighbour_offsets[i];
        if (off_point.x < 0 || off_point.y < 0 || 
                off_point.x >= corpus.width || off_point.y >= corpus.height ||
                !*corpus_mask.at(off_point)) {
            sum += 255*input_bytes + 255*map_bytes;
        } else {
            const Pixelel *corpus_pixel = corpus.at(off_point),
                          *data_pixel   = neighbour_values[i];
            if (i)
                for(int j=0;j<input_bytes;j++)
                    sum += abs(data_pixel[j] - corpus_pixel[j]);
            if (input_bytes != bytes)
                for(int j=input_bytes;j<bytes;j++)
                    sum += abs(data_pixel[j] - corpus_pixel[j]);
        }

        if (sum >= best_so_far)
            return sum;
    }
    return sum;
}
*/

static inline void try_point(const Coordinates &point)
{
    int difference = get_metric(point, best);
    if (best < difference)
        return;
    best = difference;
    best_point = point;
}


/* This is the main function. */

static void run(const gchar *name,
        gint nparams,
        const GimpParam *param,
        gint *nreturn_vals,
        GimpParam **return_vals)
{
    static GimpParam values[1];
    Parameters parameters;
    GimpDrawable *drawable, *corpus_drawable, *map_in_drawable, *map_out_drawable;
    bool ok, with_map;

    srand(time(0));
    textdomain("resynthesize") ;

    /* Unless anything goes wrong, result is success */
    *nreturn_vals = 1;
    *return_vals = values;
    values[0].type = GIMP_PDB_STATUS;
    values[0].data.d_status = GIMP_PDB_SUCCESS;

    init_gtk();

    /* Get drawable */
    drawable = gimp_drawable_get(param[2].data.d_drawable);

    if (!gimp_drawable_is_rgb(drawable->drawable_id) &&
            !gimp_drawable_is_gray(drawable->drawable_id)) {
        gimp_drawable_detach(drawable);
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Deal with run mode (parameter zero) */
    ok = false;
    switch(param[0].data.d_int32) {
        case GIMP_RUN_INTERACTIVE :
            get_last_parameters(&parameters,drawable->drawable_id); 
            ok = get_parameters_by_asking(&parameters,drawable->drawable_id); 
            break;
        case GIMP_RUN_NONINTERACTIVE :
            ok = get_parameters_from_list(&parameters, nparams, param); 
            break;
        case GIMP_RUN_WITH_LAST_VALS :
            ok = get_last_parameters(&parameters,drawable->drawable_id); 
            break;
    }

    if (!ok) {
        gimp_drawable_detach(drawable);
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }


    corpus_drawable = gimp_drawable_get(parameters.corpus_id);
    if (corpus_drawable->bpp != drawable->bpp) {
        gimp_message(_("The input texture and output image must have the same number of color channels."));
        gimp_drawable_detach(drawable);
        gimp_drawable_detach(corpus_drawable);
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    with_map = (parameters.input_map_id != -1 && parameters.output_map_id != -1);
    map_in_drawable=0;
    map_out_drawable=0;

    if (with_map) {
        map_in_drawable = gimp_drawable_get(parameters.input_map_id);
        map_out_drawable = gimp_drawable_get(parameters.output_map_id);

        if (map_in_drawable->bpp != map_out_drawable->bpp) {
            gimp_message(_("The input and output maps must have the same number of color channels"));
            ok = false;
        } else if (map_in_drawable->width != corpus_drawable->width || 
                map_in_drawable->height != corpus_drawable->height) {
            gimp_message(_("The input map should be the same size as the input texture image"));
            ok = false;
        } else if (map_out_drawable->width != drawable->width || 
                map_out_drawable->height != drawable->height) {
            gimp_message(_("The output map should be the same size as the output image"));
            ok = false;
        }

        if (!ok) {
            gimp_drawable_detach(drawable);
            gimp_drawable_detach(corpus_drawable);
            gimp_drawable_detach(map_in_drawable);
            gimp_drawable_detach(map_out_drawable);
            values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
            return;
        }
    }

    /* Store parameters deep in the bowels of the GIMP */
    if (parameters.corpus_id == drawable->drawable_id)
        parameters.corpus_id = -1;

    if (param[0].data.d_int32 == GIMP_RUN_INTERACTIVE) {
        gimp_set_data("plug_in_resynthesizer", &parameters, sizeof(parameters));
    }

    if (parameters.corpus_id == -1)
        parameters.corpus_id = drawable->drawable_id;


    gimp_progress_init(_("Resynthesize"));
    gimp_progress_update(0.0);

    /* *********************************************************************** */
    /* Data layout (actual image and map may vary)

       Actual corpus  R
       G
       B
       A
       Map            R
       G
       B
       A
       */

    input_bytes  = drawable->bpp;
    map_bytes    = (with_map ? map_in_drawable->bpp : 0);
    map_pos      = input_bytes; //TODO: map_pos can be safely replaced with input_bytes 
    bytes        = map_pos + map_bytes;

    fetch_image_and_mask(drawable, data, bytes, data_mask, 255);

    if (with_map) {
        data.from_drawable(map_out_drawable, 0, 0, map_pos);
    }

    status.size(data.width,data.height,1);

    data_points.resize(0);

    for(int y=0;y<status.height;y++)
        for(int x=0;x<status.width;x++) {
            status.at(x,y)[0].has_source = false;
            status.at(x,y)[0].has_value = false;

            if (parameters.use_border && data_mask.at(x,y)[0] == 0)
                status.at(x,y)->has_value  = true;

            if (data_mask.at(x,y)[0] != 0)
                data_points.push_back(Coordinates(x,y));
        }

    /* Fetch the corpus */

    fetch_image_and_mask(corpus_drawable, corpus, bytes, corpus_mask, 0);

    if (with_map) {
        corpus.from_drawable(map_in_drawable, 0, 0, map_pos);
    }

    corpus_points.resize(0);

    for(int y=0;y<corpus.height;y++)
        for(int x=0;x<corpus.width;x++) {
            corpus_mask.at(x,y)[0] = 255 - corpus_mask.at(x,y)[0];
            if (corpus_mask.at(x,y)[0])
                corpus_points.push_back(Coordinates(x,y));
        }

    /* Sanity check */

    if (!corpus_points.size() || !data_points.size()) {
        if (!corpus_points.size())
            gimp_message("The input texture is too small.");
        else
            gimp_message("The output image is too small.");

        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Setup */

    FILE* logfile = fopen("/home/ethercrow/tmp/resynth.log", "wt");
    int64_t perf_neighbour_search      = 0;
    int64_t perf_refinement            = 0;
    int64_t perf_doin_it_right         = 0;
    struct timespec perf_tmp;

    make_offset_list();

    for(int i=-256;i<256;i++) {
        double value = neglog_cauchy(i/256.0/parameters.autism) 
            / neglog_cauchy(1.0/parameters.autism) * 65536.0;
        diff_table[256+i] = int(value);
        map_diff_table[256+i] = int(i*i*parameters.map_weight*4.0);
    }

    vector<Coordinates>::iterator refinement_begins_here = data_points.end();

    // this forces some data_points to be reconsidered
    // after all has been mapped
    for(int n=data_points.size();n>0;) {
        // note magic number... the more repetition, the higher the quality, maybe
        // obviously, we need this multiplier under 1 to converge
        
        n *= 7/8; 
        for(int i=0;i<n;i++)
            data_points.push_back(data_points[i]);
    }

    std::random_shuffle(data_points.begin(), refinement_begins_here);
    std::random_shuffle(refinement_begins_here, data_points.end());

    /* Do it */

    Bitmap<int> tried;
    tried.size(corpus.width,corpus.height,1);
    for(int i=0;i<corpus.width*corpus.height;i++)
        tried.data[i] = -1;

    int total_points = data_points.size();
    int points_to_go = total_points;
    vector<int> previous_pass(0);
    while (points_to_go) {
        int data_points_size = data_points.size();
        for(int i=0; points_to_go && i < data_points_size; ++i) {
            //fprintf(logfile, "\n1 %d ", points_to_go);
            //fflush(logfile);
            Coordinates position = data_points[i];

            // continue if we are far from the boundaries
            bool island_flag = true;
            for (int ox=-1; ox<=1; ++ox)
                for (int oy=-1; oy<=1; ++oy)
                    if (status.at(position + Coordinates(ox, oy))->has_value)
                        island_flag = false;

            if (island_flag)
                continue;

            //fprintf(logfile, "2");
            //fflush(logfile);

            if ((points_to_go&1023) == 0)
                gimp_progress_update(1.0-float(points_to_go)/total_points);

            best = 1<<30;

            ///////////////////////////
            // Neighbour search BEGIN
            ///////////////////////////


            neighbour_offsets.clear();
            //neighbour_statuses.clear();
            n_neighbours = 0;
            const int sorted_offsets_size = sorted_offsets.size();

            for(int j=0;j<sorted_offsets_size;j++) {
                Coordinates point = position + sorted_offsets[j];
                //fprintf(logfile, "a");
                //fflush(logfile);

                if (wrap_or_clip(parameters, data, point) && 
                        status.at(point)->has_value)
                {
                    //fprintf(logfile, "b");
                    //fflush(logfile);
                    neighbour_offsets.push_back(sorted_offsets[j]);
                    //neighbour_statuses.push_back(status.at(point));
                    //fprintf(logfile, "c");
                    //fflush(logfile);
                    for(int k=0;k<bytes;k++)
                        neighbour_values[n_neighbours][k] = data.at(point)[k];
                    //fprintf(logfile, "d");
                    //fflush(logfile);
                    n_neighbours++;
                    if (neighbour_offsets.size() >= parameters.neighbours) break;
                    //fprintf(logfile, "d");
                    //fflush(logfile);
                }
            }

            //fprintf(logfile, "3");
            //fflush(logfile);

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_neighbour_search -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            /*
            for(int j=0;j<n_neighbours && best != 0;j++)
                if (neighbour_statuses[j]->has_source) {
                    Coordinates point = neighbour_statuses[j]->source - neighbour_offsets[j];
                    if (point.x < 0 || point.y < 0 || point.x >= corpus.width || point.y >= corpus.height) continue;
                    if (!corpus_mask.at(point)[0] || tried.at(point)[0] == i) continue;
                    try_point(point);
                    tried.at(point)[0] = i;
                }

            */   

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_neighbour_search += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;


            ///////////////////////////
            // Neighbour search END
            ///////////////////////////
           
            //fprintf(logfile, "4");
            //fflush(logfile);

            ///////////////////////////
            // Random refinement BEGIN
            ///////////////////////////

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_refinement -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            for(int j=0;j<parameters.trys && best != 0;j++)
                try_point(corpus_points[rand()%corpus_points.size()]);

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_refinement += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            ///////////////////////////
            // Random refinement END 
            ///////////////////////////
            
            //fprintf(logfile, "5");
            //fflush(logfile);

            // transfer patch
            for (int ox=-2; ox<=2; ++ox) {
                for (int oy=-2; oy<=2; ++oy) {
                    Coordinates offset(ox, oy);
                    Coordinates near_pos  = position + offset;
                    Coordinates near_best = best_point + offset;
                    for(int j=0;j<input_bytes;j++)
                        data.at(near_pos)[j] = corpus.at(near_best)[j];
                    status.at(near_pos)->has_source = true;
                    status.at(near_pos)->source = near_best;
                }
            }

            //fprintf(logfile, "6");
            //fflush(logfile);

            /*
            // transfer single pixel
            for(int j=0;j<input_bytes;j++)
                data.at(position)[j] = corpus.at(best_point)[j];
            status.at(position)->has_source = true;
            status.at(position)->source = best_point;
            */

            --points_to_go;
            previous_pass.push_back(i);
        }
        //fprintf(logfile, "7");
        //fflush(logfile);
        for (int j = previous_pass.size() - 1; j >= 0; --j) {
            status.at(data_points[previous_pass[j]])->has_value = true;
            data_points.erase(data_points.begin()+previous_pass[j]);
        }
        //fprintf(logfile, "8");
        //fflush(logfile);
        if (previous_pass.empty())
            break;
        previous_pass.clear();
    }

    fprintf(logfile, "neighbour search took %lld nsec\n", perf_neighbour_search);
    fprintf(logfile, "refinement took %lld nsec\n", perf_refinement);
    fprintf(logfile, "doin it right took %lld nsec\n", perf_doin_it_right);
    fclose(logfile);


    /* Write result back to the GIMP, clean up */

    /* Write result to region */
    data.to_drawable(drawable, 0,0, 0);

    /* Voodoo to update actual image */
    gimp_drawable_flush(drawable);
    gimp_drawable_merge_shadow(drawable->drawable_id,TRUE);
    gimp_drawable_update(drawable->drawable_id,0,0,data.width,data.height);

    gimp_drawable_detach(drawable);
    gimp_drawable_detach(corpus_drawable);

    if (with_map) {
        gimp_drawable_detach(map_out_drawable);
        gimp_drawable_detach(map_in_drawable);
    }

    gimp_displays_flush();
}
