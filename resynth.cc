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

#include "esynth_types.h"
#include "esynth_consts.h"
#include "esynth_gimp_comm.h"
#include "esynth_geometry.h"

/* Helpers for the main function */

static int diff_table[512], map_diff_table[512];

static int input_bytes, map_bytes, map_pos, bytes;

// we must fill selection subset of data
// using corpus_mask subset of corpus for inspiration
// status holds current state of point filling

// corpus origin is shifted from image origin by (corpus_xoff, corpus_yoff)

static Bitmap<Pixelel> data, corpus, corpus_mask;
static Bitmap<Status> data_status;
static int corpus_xoff, corpus_yoff;

// data_points is a queue of points to be filled,
// it can contain duplicates, which mean points re-analysis
// corpus_points contains all points from corpus_mask subset of corpus
//
// WARNING: coordinates in data_points are relative to the whole image origin
// and coordinates in corpus_points are relative to corpus origin
//
// sorted_offsets is an array of points near origin, beginning with (0,0)
// and sorted by distance from origin (see Coordinates::operator< for 
// current definition of 'distance')
static vector<Coordinates> data_points, corpus_points, sorted_offsets;

// neighbour_offsets is subset of sorted_offsets
// it contains only those offsets which lead to 
// already defined points
static vector<Coordinates> neighbour_offsets(0);
static Pixelel neighbour_values[max_neighbours][8];
static vector<Status> neighbour_statuses(0);

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

static void setup_metric(float autism, float map_weight)
{
    for(int i=-256;i<256;i++) {
        double value = neglog_cauchy(i/256.0/autism) 
            / neglog_cauchy(1.0/autism) * 65536.0;
        diff_table[256+i] = int(value);
        map_diff_table[256+i] = int(i*i*map_weight*4.0);
    }
}

static int get_metric(const Coordinates& point, int best_so_far)
{
    int sum = 0;
    for(int i=neighbour_offsets.size()-1; i>=0; --i) {
        Coordinates off_point = point + neighbour_offsets[i];
        if (off_point.x < 0 || off_point.y < 0 ||
            off_point.x >= corpus.width || off_point.y >= corpus.height ||
            !corpus_mask.at(off_point)[0])
        {
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

    //////////////////////////////
    // Gimp setup dragons BEGIN
    //////////////////////////////

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

    //////////////////////////////
    // Gimp setup dragons END
    //////////////////////////////


    input_bytes  = drawable->bpp;
    map_bytes    = (with_map ? map_in_drawable->bpp : 0);
    map_pos      = input_bytes; //TODO: map_pos can be safely replaced with input_bytes 
    bytes        = map_pos + map_bytes;

    /* Fetch the whole image data */
    {
        Bitmap<Pixelel> data_mask;
        int data_xoff, data_yoff;
        fetch_image_and_mask(drawable, data, bytes, data_mask, 255, data_xoff, data_yoff);

        if (with_map) {
            data.from_drawable(map_out_drawable, 0, 0, map_pos);
        }

        data_status.size(data.width,data.height,1);

        data_points.resize(0);

        for(int y=0;y<data_status.height;y++)
            for(int x=0;x<data_status.width;x++) {
                data_status.at(x,y)[0].has_source = false;
                data_status.at(x,y)[0].has_value = false;

                if (parameters.use_border && data_mask.at(x,y)[0] == 0) {
                    data_status.at(x,y)->has_value  = true;
                }

                if (data_mask.at(x,y)[0] != 0)
                    data_points.push_back(Coordinates(x,y));
            }
    }

    /* Fetch the corpus */

    fetch_image_and_mask(corpus_drawable, corpus, bytes, corpus_mask, 0, corpus_xoff, corpus_yoff);

    if (with_map) {
        corpus.from_drawable(map_in_drawable, 0, 0, map_pos);
    }

    corpus_points.resize(0);

    for(int y=0;y<corpus.height;y++)
        for(int x=0;x<corpus.width;x++) {
            corpus_mask.at(x,y)[0] = 255 - corpus_mask.at(x,y)[0];
            if (corpus_mask.at(x,y)[0]) {
                corpus_points.push_back(Coordinates(x,y));
                data_status.at(x+corpus_xoff, y+corpus_yoff)->has_source = true;
                data_status.at(x+corpus_xoff, y+corpus_yoff)->source     = Coordinates(x, y);
            }
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

    setup_metric(parameters.autism, parameters.map_weight);

    vector<Coordinates>::iterator refinement_begins_here = data_points.end();

    /*
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

    */

    /* Do it */

    fprintf(logfile, "status  dimensions: (%d, %d)\n", data_status.width, data_status.height);
    fprintf(logfile, "data dimensions: (%d, %d)\n", data.width, data.height);
    fprintf(logfile, "corpus dimensions: (%d, %d, %d, %d)\n", corpus_xoff, corpus_yoff, corpus.width, corpus.height);
    fprintf(logfile, "Do it\n");
    fflush(logfile);

    //Bitmap<int> tried;
    //tried.size(corpus.width,corpus.height,1);
    //for(int i=0;i<corpus.width*corpus.height;i++)
    //    tried.data[i] = -1;

    int total_points = data_points.size();
    int points_to_go = total_points;
    while (points_to_go) {
        gimp_progress_update(1.0-float(points_to_go)/total_points);
        //fprintf(logfile, "\n%d", points_to_go);
        //fflush(logfile);

        // fill edge_points with points that are near already filled points
        // that ensures inward propagation
        vector<Coordinates> edge_points(0);
        for(int i=0; i < data_points.size(); ++i) {
            Coordinates position = data_points[i];
            bool island_flag = true;
            for (int ox=-1; ox<=1; ++ox)
                for (int oy=-1; oy<=1; ++oy)
                    if (data_status.at(position + Coordinates(ox, oy))->has_value)
                        island_flag = false;
            if (!island_flag) {
                edge_points.push_back(data_points[i]);
                data_points.erase(data_points.begin() + i--);
            }
        }
        random_shuffle(edge_points.begin(), edge_points.end());
        int edge_points_size = edge_points.size();

        // cover edge points with best-fit patches
        for(int i=0; i < edge_points_size; ++i) {
            fprintf(logfile, "\n1");
            fflush(logfile);
            Coordinates position = edge_points[i];

            //fprintf(logfile, "2");
            //fflush(logfile);

            best = 1<<30;

            ///////////////////////////
            // Neighbour search BEGIN
            ///////////////////////////


            neighbour_offsets.clear();
            neighbour_statuses.clear();
            int n_neighbours = 0;
            const int sorted_offsets_size = sorted_offsets.size();

            for(int j=0;j<sorted_offsets_size;j++) {
                Coordinates point = position + sorted_offsets[j];
                //fprintf(logfile, "a");
                //fflush(logfile);

                if (wrap_or_clip(parameters, data, point) && 
                        data_status.at(point)->has_source)
                {
                    //fprintf(logfile, "b");
                    //fflush(logfile);
                    neighbour_offsets.push_back(sorted_offsets[j]);
                    neighbour_statuses.push_back(*(data_status.at(point)));
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

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_neighbour_search -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            for(int j=neighbour_offsets.size() - 1; j>=0 && best != 0; --j) {
                Coordinates point = neighbour_statuses[j].source - neighbour_offsets[j];
                if (neighbour_statuses[j].has_source) {
                    if (point.x < 0 || point.y < 0 || point.x >= corpus.width || point.y >= corpus.height) continue;
                    if (!corpus_mask.at(point)[0]) continue;
                    //if(tried.at(point)[0] == points_to_go) continue;
                    try_point(point);
                    //tried.at(point)[0] = points_to_go;
                }
            }

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

            /*
            // transfer patch
            for (int ox=-1; ox<=1; ++ox) {
                for (int oy=-1; oy<=1; ++oy) {
                    Coordinates offset(ox, oy);
                    Coordinates near_pos  = position + offset;
                    Coordinates near_best = best_point + offset;
                    for(int j=0;j<input_bytes;j++)
                        data.at(near_pos)[j] = corpus.at(near_best)[j];
                    status.at(near_pos)->has_source = true;
                    status.at(near_pos)->source = near_best;
                }
            }
            */

            // transfer single pixel
            for(int j=0;j<input_bytes;j++)
                data.at(position)[j] = corpus.at(best_point)[j];
            data_status.at(position)->has_source = true;
            data_status.at(position)->has_value  = true;
            data_status.at(position)->source     = best_point;

            //fprintf(logfile, "6");
            //fflush(logfile);

            --points_to_go;
        }
        if (!edge_points_size)
            break;
        //fprintf(logfile, "6");
        //fflush(logfile);
    }

    fprintf(logfile, "\nneighbour search took %lld usec\n", perf_neighbour_search/1000);
    fprintf(logfile, "refinement took %lld usec\n", perf_refinement/1000);
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
