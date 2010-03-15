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
#include <utility>
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
static int comp_patch_radius, transfer_patch_radius;

// we must fill selection subset of data
// using corpus_mask subset of corpus for inspiration
// status holds current state of point filling

// corpus origin is shifted from image origin by (corpus_xoff, corpus_yoff)

        
static Bitmap<Pixelel> data, data_mask, corpus, corpus_mask;
static Bitmap<Status> data_status;
static int sel_x1, sel_y1, sel_x2, sel_y2;
static int corpus_xoff, corpus_yoff;

// data_points is a queue of points to be filled,
// it can contain duplicates, which mean points re-analysis
//
// sorted_offsets is an array of points near origin, beginning with (0,0)
// and sorted by distance from origin (see Coordinates::operator< for 
// current definition of 'distance')
static vector<Coordinates> data_points, sorted_offsets;

static int best;
static Coordinates best_point;

static double neglog_cauchy(double x) {
    return log(x*x+1.0);
}

static void make_offset_list(void) {
    sorted_offsets.resize(0);
    int half_side = sqrt(max_neighbours);
    for(int y=-half_side-1; y<half_side+1; ++y)
        for(int x=-half_side-1; x<half_side+1; ++x)
            sorted_offsets.push_back(Coordinates(x,y));

    sort(sorted_offsets.begin(), sorted_offsets.end());
}

static void setup_metric(float autism, float map_weight)
{
    // TODO: improve patch difference metric
    for(int i=-256;i<256;i++) {
        //double value = neglog_cauchy(i/256.0/autism) / neglog_cauchy(1.0/autism) * 65536.0;
        //diff_table[256+i] = int(value);
        diff_table[256+i] = i*i;
        map_diff_table[256+i] = int(i*i*map_weight*4.0);
    }
}

// structural complexity of point's neighbourhood
static int get_complexity(const Coordinates& point)
{
    // TODO: improve complexity metric
    int confidence_sum = 0;

    // calculate std dev
    // TODO: optimize
    vector<Coordinates> defined_points(0);
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            Coordinates point_off = point + Coordinates(ox, oy);
            if (data_status.at(point_off)->confidence) {
                confidence_sum += data_status.at(point_off)->confidence;
                defined_points.push_back(point_off);
            }
        }

    int defined_count = defined_points.size();

    int mean_values[input_bytes];
    for (int j = 0; j<input_bytes; ++j)
        mean_values[j] = 0;
    for (int i = 0; i<defined_count; ++i) {
        Pixelel* colors = data.at(defined_points[i]);
        for (int j = 0; j<input_bytes; ++j)
            mean_values[j] = colors[j];
    }

    int stddev[input_bytes];
    for (int j = 0; j<input_bytes; ++j)
        stddev[j] = 0;
    for (int i = 0; i<defined_count; ++i) {
        Pixelel* colors = data.at(defined_points[i]);
        for (int j = 0; j<input_bytes; ++j) {
            int c = mean_values[j] - colors[j];
            stddev[j] += c*c;
        }
    }

    int result = 0;
    for (int j = 0; j<input_bytes; ++j)
        result += stddev[j];
    result /= defined_count;

    // multiply by average confidence among defined points
    result *= (confidence_sum/defined_count);

    return result;
}

static int get_difference(const Coordinates& candidate, const Coordinates& position)
{
    int sum = 0;
    int compared_count = 0;
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            // TODO: consider mirroring and rotation
            Coordinates off = Coordinates(ox, oy);
            Coordinates candidate_off = candidate + off;
            Coordinates position_off  = position  + off;
            if (data_status.at(candidate_off)->confidence && 
                data_status.at(position_off)->confidence)
            {
                ++compared_count;
                for(int j=0;j<input_bytes;j++) {
                    int d = (data.at(position_off)[j] - data.at(candidate_off)[j]);
                    sum += diff_table[256 + d];
                }
            }  else if (!data_status.at(candidate_off)->confidence)
            {
                sum += diff_table[0];
            }
        }
    if (compared_count)
        return sum;
    else
        return best+1;
}

static inline void try_point(const Coordinates& candidate, const Coordinates& position)
{
    int difference = get_difference(candidate, position);
    if (best < difference)
        return;
    best = difference;
    best_point = candidate;
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

    FILE* logfile = fopen("/tmp/resynth.log", "wt");

    fprintf(logfile, "gimp setup dragons begin");
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

    fprintf(logfile, "init_gtk");
    init_gtk();

    /* Get drawable */
    fprintf(logfile, "get drawable\n");
    drawable = gimp_drawable_get(param[2].data.d_drawable);

    if (!gimp_drawable_is_rgb(drawable->drawable_id) &&
            !gimp_drawable_is_gray(drawable->drawable_id)) {
        gimp_drawable_detach(drawable);
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Deal with run mode (parameter zero) */
    fprintf(logfile, "before get_parameters %d\n", param[0].data.d_int32);
    ok = false;
    switch(param[0].data.d_int32) {
        case GIMP_RUN_NONINTERACTIVE :
            ok = get_parameters_from_list(&parameters, nparams, param); 
            break;
        case GIMP_RUN_WITH_LAST_VALS :
            ok = get_last_parameters(&parameters,drawable->drawable_id); 
            break;
    }
    fprintf(logfile, "after get_parameters\n");

    if (!ok) {
        fprintf(logfile, "Panic!\n");
        gimp_drawable_detach(drawable);
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }


    corpus_drawable = gimp_drawable_get(parameters.corpus_id);
    if (corpus_drawable->bpp != drawable->bpp) {
        fprintf(logfile, "get corpus failed");
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

    if (parameters.corpus_id == -1)
        parameters.corpus_id = drawable->drawable_id;

    gimp_progress_init(_("Resynthesize"));
    gimp_progress_update(0.0);

    fprintf(logfile, "gimp setup dragons end\n");
    //////////////////////////////
    // Gimp setup dragons END
    //////////////////////////////

    comp_patch_radius     = parameters.comp_size;
    transfer_patch_radius = parameters.transfer_size;
    input_bytes  = drawable->bpp;
    map_bytes    = (with_map ? map_in_drawable->bpp : 0);
    map_pos      = input_bytes; //TODO: map_pos can be safely replaced with input_bytes 
    bytes        = map_pos + map_bytes;

    /* Fetch the whole image data */
    {
        fetch_image_and_mask(drawable, data, bytes, data_mask, 255, sel_x1, sel_y1, sel_x2, sel_y2);

        if (with_map)
            data.from_drawable(map_out_drawable, 0, 0, map_pos);

        data_status.size(data.width,data.height,1);

        data_points.resize(0);

        for(int y=0;y<data_status.height;y++)
            for(int x=0;x<data_status.width;x++) {
                data_status.at(x,y)->confidence = 0;

                if (parameters.use_border && data_mask.at(x,y)[0] == 0)
                    data_status.at(x,y)->confidence = 255;

                if (data_mask.at(x,y)[0] != 0)
                    data_points.push_back(Coordinates(x,y));
            }
    }

    /* Fetch the corpus */

    fetch_image_and_mask(corpus_drawable, corpus, bytes, corpus_mask, 0);

    if (with_map)
        corpus.from_drawable(map_in_drawable, 0, 0, map_pos);

    /* there's some weird convention about corpus selection inversion. For now. */
    for(int y=0;y<corpus.height;y++)
        for(int x=0;x<corpus.width;x++)
            corpus_mask.at(x,y)[0] = 255 - corpus_mask.at(x,y)[0];

    /* little geometry so that sel_x1 and sel_y1 are now corpus_offset */

    if (sel_x2 >= data.width - max(comp_patch_radius, transfer_patch_radius))
        sel_x2 = data.width - max(comp_patch_radius, transfer_patch_radius) - 1;

    if (sel_y2 >= data.height - max(comp_patch_radius, transfer_patch_radius))
        sel_y2 = data.height - max(comp_patch_radius, transfer_patch_radius) - 1;

    sel_x1 -= (corpus.width  - (sel_x2-sel_x1))/2;
    sel_x1 = max(max(comp_patch_radius, transfer_patch_radius), sel_x1);
    sel_y1 -= (corpus.height - (sel_y2-sel_y1))/2;
    sel_y1 = max(max(comp_patch_radius, transfer_patch_radius), sel_y1);

    sel_x2 = min(sel_x1 + corpus.width, data.width - max(comp_patch_radius, transfer_patch_radius) - 1);
    sel_y2 = min(sel_y1 + corpus.height, data.height - max(comp_patch_radius, transfer_patch_radius) - 1);

    /* Sanity check */

    if (!data_points.size()) {
        gimp_message("The output image is too small.");
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Setup */
    int64_t perf_neighbour_search = 0;
    int64_t perf_refinement       = 0;
    int64_t perf_fill_undo        = 0;
    int64_t perf_edge_points      = 0;
    struct timespec perf_tmp;

    make_offset_list();

    setup_metric(parameters.autism, parameters.map_weight);

    vector<Coordinates>::iterator refinement_begins_here = data_points.end();

    // this forces some data_points to be reconsidered
    // after all has been mapped
    for(int n=data_points.size();n>0;) {
        // note magic number... the more repetition, the higher the quality, maybe
        // obviously, we need this multiplier under 1 to converge
        
        n *= 1/4; 
        for(int i=0;i<n;i++)
            data_points.push_back(data_points[i]);
    }

    std::random_shuffle(data_points.begin(), refinement_begins_here);
    std::random_shuffle(refinement_begins_here, data_points.end());

    /* Do it */

    fprintf(logfile, "status  dimensions: (%d, %d)\n", data_status.width, data_status.height);
    fprintf(logfile, "data dimensions: (%d, %d)\n", data.width, data.height);
    fprintf(logfile, "corpus dimensions: (%d, %d, %d, %d)\n", sel_x1, sel_y1, sel_x2-sel_x1, sel_y2-sel_y1);
    fflush(logfile);

    int total_points = data_points.size();
    int points_to_go = total_points;
    while (points_to_go > 0) {
        gimp_progress_update(1.0-float(points_to_go)/total_points);
        //fprintf(logfile, "\n%d", points_to_go);
        //fflush(logfile);
        

        // fill edge_points with points that are near already filled points
        // that ensures inward propagation
        // first element in pair is complexity of point neighbourhood
        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_edge_points -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        vector<pair<int, Coordinates> > edge_points(0);
        for(int i=0; i < data_points.size(); ++i) {
            Coordinates position = data_points[i];

            // check if this point has been already set by some patch
            if (data_status.at(position)->confidence) {
                data_points.erase(data_points.begin() + i--);
                --points_to_go;
                continue;
            }

            bool island_flag = true;
            for (int ox=-1; ox<=1; ++ox)
                for (int oy=-1; oy<=1; ++oy)
                    if (data_status.at(position + Coordinates(ox, oy))->confidence)
                        island_flag = false;
            if (!island_flag) {
                edge_points.push_back(std::make_pair(get_complexity(data_points[i]),
                    data_points[i]));
            }
        }
        sort(edge_points.begin(), edge_points.end());

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_edge_points += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        // leave only the most important edge_points
        if (edge_points.size() > important_count)
            edge_points.erase(edge_points.begin(), edge_points.end()-edge_points.size()/5);

        int edge_points_size = edge_points.size();

        // find best-fit patches for edge_points
        for(int i=0; i < edge_points_size; ++i) {
            //fprintf(logfile, "\n1");
            //fflush(logfile);
            Coordinates position = edge_points[i].second;
            if (data_status.at(position)->confidence)
                continue;

            //fprintf(logfile, "2");
            //fflush(logfile);

            best = 1<<30;

            ///////////////////////////
            // Neighbour search BEGIN
            ///////////////////////////
            
            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_neighbour_search -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            int n_neighbours = 0;
            const int sorted_offsets_size = sorted_offsets.size();

            for(int j=0;j<sorted_offsets_size;j++) {
                Coordinates candidate = position + sorted_offsets[j];
                if (wrap_or_clip(parameters, data, candidate) && 
                        data_status.at(candidate)->confidence)
                {
                    try_point(candidate, position);
                    if (++n_neighbours >= parameters.neighbours) break;
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

            for (int j=0; j<parameters.tries; ++j) {
                int x, y;
                // FIXME: this will suck with large rectangular selections with small borders
                do {
                    x = sel_x1 + rand()%(sel_x2 - sel_x1);
                    y = sel_y1 + rand()%(sel_y2 - sel_y1);
                } while (data_mask.at(x,y)[0]);
                try_point(Coordinates(x, y), position);
            }

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_refinement += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            ///////////////////////////
            // Random refinement END 
            ///////////////////////////
            
            //fprintf(logfile, "5");
            //fflush(logfile);

            for (int ox=-transfer_patch_radius; ox<=transfer_patch_radius; ++ox) {
                for (int oy=-transfer_patch_radius; oy<=transfer_patch_radius; ++oy) {
                    Coordinates offset(ox, oy);
                    Coordinates near_dst = position + offset;
                    Coordinates near_src = best_point + offset;
                    // transfer only defined points and only to undefined points
                    if (!(data_status.at(near_dst)->confidence) &&
                        data_status.at(near_src)->confidence)
                    {
                        for(int j=0;j<input_bytes;j++)
                            data.at(near_dst)[j] = data.at(near_src)[j];
                        // TODO: better confidence transfer
                        data_status.at(near_dst)->confidence = max(10, data_status.at(near_src)->confidence - 5); 
                    }
                }
            }

            //fprintf(logfile, "6");
            //fflush(logfile);

        }

        if (!edge_points_size)
            break;
        //fprintf(logfile, "6");
        //fflush(logfile);
        
        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_fill_undo -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        /* Write result to region */
        data.to_drawable(drawable, 0,0, 0);

        /* Voodoo to update actual image */
        gimp_drawable_flush(drawable);
        gimp_drawable_merge_shadow(drawable->drawable_id,TRUE);
        gimp_drawable_update(drawable->drawable_id,0,0,data.width,data.height);

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_fill_undo += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;
    }

    fprintf(logfile, "\n%d points left unfilled\n", points_to_go);
    fprintf(logfile, "populating edge_points took %lld usec\n", perf_edge_points/1000);
    fprintf(logfile, "neighbour search took %lld usec\n", perf_neighbour_search/1000);
    fprintf(logfile, "refinement took %lld usec\n", perf_refinement/1000);
    fprintf(logfile, "updating undo stack took %lld usec\n", perf_fill_undo/1000);
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
