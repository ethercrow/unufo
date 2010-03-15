#ifndef ESYNTH_GIMP_COMM_H
#define ESYNTH_GIMP_COMM_H

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

/* Inclusion : Laurent Despeyroux
   This is for other GNU distributions with internationalized messages.
   When compiling libc, the "_" macro is predefined.  */
#ifndef _
#include <libintl.h>
#define _(msgid)    gettext (msgid)
#endif

//Get a drawable and possibly its selection mask from the GIMP
void fetch_image_and_mask(GimpDrawable *drawable, Bitmap<Pixelel> &image, int bytes, 
        Bitmap<Pixelel> &mask, Pixelel default_mask_value,
        int& sel_x1, int& sel_y1, int& sel_x2, int& sel_y2)
{
    int x,y, xoff, yoff;
    Bitmap<Pixelel> temp_mask;
    int sel_id;
    int has_selection;
    GimpDrawable *mask_drawable;

    image.size(drawable->width, drawable->height, bytes);
    mask.size(drawable->width, drawable->height, 1);

    image.from_drawable(drawable,0,0,0);

    has_selection = gimp_drawable_mask_bounds(drawable->drawable_id,
            &sel_x1, &sel_y1, &sel_x2, &sel_y2);
    gimp_drawable_offsets(drawable->drawable_id, &xoff, &yoff);

    if (!has_selection) {
        memset(mask.data, default_mask_value, mask.width*mask.height*sizeof(Pixelel));
        return;
    }

    temp_mask.size(sel_x2-sel_x1, sel_y2-sel_y1, 1);

    sel_id = gimp_image_get_selection(gimp_drawable_get_image(drawable->drawable_id));
    mask_drawable = gimp_drawable_get(sel_id);


    temp_mask.from_drawable(mask_drawable, sel_x1+xoff, sel_y1+yoff, 0);

    gimp_drawable_detach(mask_drawable);

    memset(mask.data, 0, mask.width*mask.height*sizeof(Pixelel));
    for(y=0;y<temp_mask.height;y++)
        for(x=0;x<temp_mask.width;x++)
            mask.at(x+sel_x1,y+sel_y1)[0] = temp_mask.at(x,y)[0];
}

void fetch_image_and_mask(GimpDrawable *drawable, Bitmap<Pixelel> &image, int bytes, 
        Bitmap<Pixelel> &mask, Pixelel default_mask_value)
{
    int x1, y1, x2, y2;
    fetch_image_and_mask(drawable, image, bytes, mask, default_mask_value, x1, y1, x2, y2);
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

static void run(const gchar *name,
        gint nparams,
        const GimpParam *param,
        gint *nreturn_vals,
        GimpParam **return_vals);

static void query();
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


#endif //ESYNTH_GIMP_COMM_H

