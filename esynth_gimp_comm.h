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

/* Restore the last parameters used from deep in the bowels of
   the main GIMP app */
static bool get_last_parameters(Parameters *param, int default_drawable) {
    /* Defaults in case this is our first run */
    param->corpus_id     = -1;
    param->input_map_id  = -1;
    param->output_map_id = -1;
    param->v_tile        = true;
    param->h_tile        = true;
    param->use_border    = true;
    param->map_weight    = 0.5;
    param->autism        = 0.117; /* 30/256 */
    param->neighbours    = 30;
    param->tries         = 200;
    param->comp_size     = 3;
    param->transfer_size = 2;

    gimp_get_data("plug_in_resynthesizer", param);

    /* If image was resynthesized from itself last time, resythesize this 
       image from itself too */
    if (param->corpus_id == -1)
        param->corpus_id = default_drawable;

    return true;
}

/* Convert argument list into parameters */
static bool get_parameters_from_list(Parameters *param, int n_args, const GimpParam *args) {
    if (n_args != 15)
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
    param->tries         = args[12].data.d_int32;
    param->comp_size     = args[13].data.d_int32;
    param->transfer_size = args[14].data.d_int32;

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
        { GIMP_PDB_INT32, "tries", "Search thoroughness" },
        { GIMP_PDB_INT32, "comp_size", "Patches of this size are compared" },
        { GIMP_PDB_INT32, "transfer_size", "Size of transfer unit" },
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

