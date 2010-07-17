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

const int WORK_LAYER_PARAM_ID = 2;
const int REF_LAYER_PARAM_ID = 11;

//Get a drawable and possibly its selection mask from the GIMP
void fetch_image_and_mask(GimpDrawable *drawable, Bitmap<uint8_t> &image, int bytes, 
        Bitmap<uint8_t> &mask, uint8_t default_mask_value,
        int& sel_x1, int& sel_y1, int& sel_x2, int& sel_y2)
{
    int x,y, xoff, yoff;
    Bitmap<uint8_t> temp_mask;
    int sel_id;
    int has_selection;
    GimpDrawable *mask_drawable;

    image.resize(drawable->width, drawable->height, bytes);
    mask.resize(drawable->width, drawable->height, 1);

    image.from_drawable(drawable,0,0,0);

    has_selection = gimp_drawable_mask_bounds(drawable->drawable_id,
            &sel_x1, &sel_y1, &sel_x2, &sel_y2);
    gimp_drawable_offsets(drawable->drawable_id, &xoff, &yoff);

    if (!has_selection) {
        memset(mask.data, default_mask_value, mask.width*mask.height*sizeof(uint8_t));
        return;
    }

    temp_mask.resize(sel_x2-sel_x1, sel_y2-sel_y1, 1);

    sel_id = gimp_image_get_selection(gimp_drawable_get_image(drawable->drawable_id));
    mask_drawable = gimp_drawable_get(sel_id);


    temp_mask.from_drawable(mask_drawable, sel_x1+xoff, sel_y1+yoff, 0);

    gimp_drawable_detach(mask_drawable);

    memset(mask.data, 0, mask.width*mask.height*sizeof(uint8_t));
    for(y=0;y<temp_mask.height;y++)
        for(x=0;x<temp_mask.width;x++)
            mask.at(x+sel_x1,y+sel_y1)[0] = temp_mask.at(x,y)[0];
}

void fetch_image_and_mask(GimpDrawable *drawable, Bitmap<uint8_t> &image, int bytes, 
        Bitmap<uint8_t> &mask, uint8_t default_mask_value)
{
    int x1, y1, x2, y2;
    fetch_image_and_mask(drawable, image, bytes, mask, default_mask_value, x1, y1, x2, y2);
}

/* Convert argument list into parameters */
static bool get_parameters_from_list(Parameters *param, int n_args, const GimpParam *args)
{
    if (n_args != 12)
        return false;

    param->corpus_id        = args[3].data.d_int32;
    param->tries            = args[4].data.d_int32;
    param->comp_size        = args[5].data.d_int32;
    param->transfer_size    = args[6].data.d_int32;
    param->invent_gradients = args[7].data.d_int32;
    param->max_adjustment   = args[8].data.d_int32;
    param->equal_adjustment = args[9].data.d_int32;
    param->use_ref_layer    = args[10].data.d_int32;

    return true;
}

/* Talking to GIMP stuff */

static void run(const gchar *name,
    gint nparams,
    const GimpParam *param,
    gint *nreturn_vals,
    GimpParam **return_vals);

/* Add capabilities of this plugin to Procedural DataBase */
static void query()
{
    static GimpParamDef args[] = {
        { GIMP_PDB_INT32, "run_mode", "Interactive, non-interactive" },
        { GIMP_PDB_IMAGE, "image", "Input image" },
        { GIMP_PDB_DRAWABLE, "drawable", "Input drawable" },

        // { GIMP_PDB_INT32, "vtile", "Make tilable vertically" },
        // { GIMP_PDB_INT32, "htile", "Make tilable horizontally" },
        // { GIMP_PDB_INT32, "noborder", "Dont change border pixels" },
        { GIMP_PDB_INT32, "corpus", "Layer to use as corpus" },
        // { GIMP_PDB_INT32, "inmask", "Layer to use as input mask, -1 for none" },
        // { GIMP_PDB_INT32, "outmask", "Layer to use as output mask, -1 for none" },
        // { GIMP_PDB_FLOAT, "map_weight", "Weight to give to map, if map is used" },
        // { GIMP_PDB_FLOAT, "autism", "Sensitivity to outliers" },
        // { GIMP_PDB_INT32, "neighbourhood", "Neighbourhood size" },
        { GIMP_PDB_INT32, "tries", "Search thoroughness" },
        { GIMP_PDB_INT32, "comp_size", "Patches of this size are compared" },
        { GIMP_PDB_INT32, "transfer_size", "Size of transfer unit" },
        { GIMP_PDB_INT32, "invent_gradients", "Invent gradients" },
        { GIMP_PDB_INT32, "max_adjustment", "Max color adjustment applied to transferred patch" },
        { GIMP_PDB_INT32, "equal_adjustment", "Adjust only overall brightness, not separate colors (expect weird alpha)" },
        { GIMP_PDB_INT32, "use_ref_layer", "Use manually defined reference area" },

        { GIMP_PDB_DRAWABLE, "ref_layer_id", "Reference map layer" }
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

static GimpPlugInInfo PLUG_IN_INFO = {
    NULL, /* init_proc */
    NULL, /* quit_proc */
    query, /* query_proc */
    run, /* run_proc */
};

#endif //ESYNTH_GIMP_COMM_H

