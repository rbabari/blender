/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2017 by Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup draw
 *
 * \brief Volume API for render engines
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "DNA_object_types.h"
#include "DNA_volume_types.h"

#include "BKE_global.h"
#include "BKE_volume.h"

#include "GPU_batch.h"
#include "GPU_texture.h"

#include "DRW_render.h"

#include "draw_cache.h"      /* own include */
#include "draw_cache_impl.h" /* own include */

static void volume_batch_cache_clear(Volume *volume);

/* ---------------------------------------------------------------------- */
/* Volume GPUBatch Cache */

typedef struct VolumeBatchCache {
  ListBase grids;

  /* settings to determine if cache is invalid */
  bool is_dirty;
} VolumeBatchCache;

/* GPUBatch cache management. */

static bool volume_batch_cache_valid(Volume *volume)
{
  VolumeBatchCache *cache = volume->batch_cache;
  return (cache && cache->is_dirty == false);
}

static void volume_batch_cache_init(Volume *volume)
{
  VolumeBatchCache *cache = volume->batch_cache;

  if (!cache) {
    cache = volume->batch_cache = MEM_callocN(sizeof(*cache), __func__);
  }
  else {
    memset(cache, 0, sizeof(*cache));
  }

  cache->is_dirty = false;
}

void DRW_volume_batch_cache_validate(Volume *volume)
{
  if (!volume_batch_cache_valid(volume)) {
    volume_batch_cache_clear(volume);
    volume_batch_cache_init(volume);
  }
}

static VolumeBatchCache *volume_batch_cache_get(Volume *volume)
{
  DRW_volume_batch_cache_validate(volume);
  return volume->batch_cache;
}

void DRW_volume_batch_cache_dirty_tag(Volume *volume, int mode)
{
  VolumeBatchCache *cache = volume->batch_cache;
  if (cache == NULL) {
    return;
  }
  switch (mode) {
    case BKE_VOLUME_BATCH_DIRTY_ALL:
      cache->is_dirty = true;
      break;
    default:
      BLI_assert(0);
  }
}

static void volume_batch_cache_clear(Volume *volume)
{
  VolumeBatchCache *cache = volume->batch_cache;
  if (!cache) {
    return;
  }

  for (DRWVolumeGrid *grid = cache->grids.first; grid; grid = grid->next) {
    MEM_SAFE_FREE(grid->name);
    DRW_TEXTURE_FREE_SAFE(grid->texture);
  }
  BLI_freelistN(&cache->grids);
}

void DRW_volume_batch_cache_free(Volume *volume)
{
  volume_batch_cache_clear(volume);
  MEM_SAFE_FREE(volume->batch_cache);
}

static DRWVolumeGrid *volume_grid_cache_get(Volume *volume,
                                            VolumeBatchCache *cache,
                                            const char *name)
{
  /* Return cached grid. */
  DRWVolumeGrid *cache_grid;
  for (cache_grid = cache->grids.first; cache_grid; cache_grid = cache_grid->next) {
    if (STREQ(cache_grid->name, name)) {
      return cache_grid;
    }
  }

  /* Allocate new grid. */
  cache_grid = MEM_callocN(sizeof(DRWVolumeGrid), __func__);
  cache_grid->name = BLI_strdup(name);
  BLI_addtail(&cache->grids, cache_grid);

  // TODO: avoid global access, load earlier?
  BKE_volume_load(volume, G.main);

  /* Find grid with matching name. */
  int num_grids = BKE_volume_num_grids(volume);
  int grid_index = 0;
  for (; grid_index < num_grids; grid_index++) {
    const VolumeGrid *grid = BKE_volume_grid_for_metadata(volume, grid_index);
    if (STREQ(BKE_volume_grid_name(grid), name)) {
      break;
    }
  }

  if (grid_index == num_grids) {
    return cache_grid;
  }

  /* Read dense voxels from grid. */
  const VolumeGrid *grid = BKE_volume_grid_for_tree(volume, grid_index);
  float *voxels = BKE_volume_grid_to_dense_voxels(grid, cache_grid->resolution);
  if (voxels == NULL) {
    return cache_grid;
  }

  /* Create GPU texture. */
  /* TODO: support multiple data types. */
  cache_grid->texture = GPU_texture_create_nD(cache_grid->resolution[0],
                                              cache_grid->resolution[1],
                                              cache_grid->resolution[2],
                                              3,
                                              voxels,
                                              GPU_R8,
                                              GPU_DATA_FLOAT,
                                              0,
                                              true,
                                              NULL);

  GPU_texture_bind(cache_grid->texture, 0);
  GPU_texture_swizzle_channel_rrrr(cache_grid->texture);
  GPU_texture_unbind(cache_grid->texture);

  MEM_freeN(voxels);

  /* Compute transform. */
  /* TODO: support full transform, compute bbox as part of dense conversion for perfomance. */
  float min[3], max[3];
  BKE_volume_grid_bounds(grid, min, max);
  copy_v3_v3(cache_grid->loc, min);
  sub_v3_v3v3(cache_grid->size, max, min);
  mid_v3_v3v3(cache_grid->mid, min, max);
  cache_grid->halfsize[0] = (max[0] - min[0]) / 2.0f;
  cache_grid->halfsize[1] = (max[1] - min[1]) / 2.0f;
  cache_grid->halfsize[2] = (max[2] - min[2]) / 2.0f;

  return cache_grid;
}

DRWVolumeGrid *DRW_volume_batch_cache_get_grid(Volume *volume, const char *name)
{
  VolumeBatchCache *cache = volume_batch_cache_get(volume);
  DRWVolumeGrid *grid = volume_grid_cache_get(volume, cache, name);
  return (grid->texture != NULL) ? grid : NULL;
}

int DRW_volume_material_count_get(Volume *volume)
{
  return max_ii(1, volume->totcol);
}
