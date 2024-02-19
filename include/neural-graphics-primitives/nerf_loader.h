/*
 * Copyright (c) 2020-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/** @file   nerf_loader.h
 *  @author Alex Evans, NVIDIA
 *  @brief  Ability to load nerf datasets.
 */

#pragma once

#include <neural-graphics-primitives/bounding_box.cuh>
#include <neural-graphics-primitives/common_host.h>
#include <neural-graphics-primitives/nerf_device.cuh>

#include <filesystem/path.h>

#include <vector>

namespace ngp {

// how much to scale the scene by vs the original nerf dataset; we want to fit the thing in the unit cube
static constexpr float NERF_SCALE = 0.33f;

inline size_t image_type_size(EImageDataType type) {
	switch (type) {
		case EImageDataType::None: return 0;
		case EImageDataType::Byte: return 1;
		case EImageDataType::Half: return 2;
		case EImageDataType::Float: return 4;
		default: return 0;
	}
}

inline size_t depth_type_size(EDepthDataType type) {
	switch (type) {
		case EDepthDataType::UShort: return 2;
		case EDepthDataType::Float: return 4;
		default: return 0;
	}
}

struct LoadedImageInfo {
	ivec2 res = ivec2(0);
	bool image_data_on_gpu = false;
	EImageDataType image_type = EImageDataType::None;
	bool white_transparent = false;
	bool black_transparent = false;
	uint32_t mask_color = 0;
	void *pixels = nullptr;
	uint16_t *depth_pixels = nullptr;
	Ray *rays = nullptr;
	float depth_scale = -1.f;
};

struct NerfDataset {
	bool is_same(const NerfDataset& other) {
		return xforms == other.xforms && paths == other.paths;
	}

	std::vector<GPUMemory<Ray>> raymemory;
	std::vector<GPUMemory<uint8_t>> pixelmemory;
	std::vector<GPUMemory<float>> depthmemory;

	std::vector<TrainingImageMetadata> metadata;
	GPUMemory<TrainingImageMetadata> metadata_gpu;

	void update_metadata(int first = 0, int last = -1);

	std::vector<std::unique_ptr<TrainingXForm>> xforms;
	std::vector<std::string> paths;
	GPUMemory<float> sharpness_data;
	ivec2 sharpness_resolution = {0, 0};
	GPUMemory<float> envmap_data;

	LoadedImageInfo info = {};

	BoundingBox render_aabb = {};
	mat3 render_aabb_to_local = mat3::identity();
	vec3 up = {0.0f, 1.0f, 0.0f};
	vec3 offset = {0.0f, 0.0f, 0.0f};
	size_t n_images = 0;
	ivec2 envmap_resolution = {0, 0};
	float scale = 1.0f;
	int aabb_scale = 1;
	bool from_mitsuba = false;
	bool fix_premult = false;
	bool enable_ray_loading = true;
	bool enable_depth_loading = true;
	bool is_hdr = false;
	float sharpen_amount;
	bool wants_importance_sampling = true;
	bool has_rays = false;

	Lens lens;
	vec2 principal_point;
	vec4 rolling_shutter;

	uint32_t n_extra_learnable_dims = 0;
	bool has_light_dirs = false;

	uint32_t n_extra_dims() const {
		return (has_light_dirs ? 3u : 0u) + n_extra_learnable_dims;
	}

	void add_training_image(nlohmann::json frame, uint8_t *img, uint16_t *depth, uint8_t *alpha, uint8_t *mask);
	void set_training_image(int frame_idx, const ivec2& image_resolution, const void* pixels, const void* depth_pixels, float depth_scale, bool image_data_on_gpu, EImageDataType image_type, EDepthDataType depth_type, float sharpen_amount = 0.f, bool white_transparent = false, bool black_transparent = false, uint32_t mask_color = 0, const Ray *rays = nullptr);

	vec3 nerf_direction_to_ngp(const vec3& nerf_dir) {
		vec3 result = nerf_dir;
		if (from_mitsuba) {
			result *= -1.0f;
		} else {
			result = vec3{result.y, result.z, result.x};
		}
		return result;
	}

	mat4x3 nerf_matrix_to_ngp(const mat4x3& nerf_matrix, bool scale_columns = false) const {
		mat4x3 result = nerf_matrix;
		result[0] *= scale_columns ? scale : 1.f;
		result[1] *= scale_columns ? -scale : -1.f;
		result[2] *= scale_columns ? -scale : -1.f;
		result[3] = result[3] * scale + offset;

		if (from_mitsuba) {
			result[0] *= -1.0f;
			result[2] *= -1.0f;
		} else {
			// Cycle axes xyz<-yzx
			vec4 tmp = row(result, 0);
			result = row(result, 0, row(result, 1));
			result = row(result, 1, row(result, 2));
			result = row(result, 2, tmp);
		}

		return result;
	}

	mat4x3 ngp_matrix_to_nerf(const mat4x3& ngp_matrix, bool scale_columns = false) const {
		mat4x3 result = ngp_matrix;
		if (from_mitsuba) {
			result[0] *= -1.0f;
			result[2] *= -1.0f;
		} else {
			// Cycle axes xyz->yzx
			vec4 tmp = row(result, 0);
			result = row(result, 0, row(result, 2));
			result = row(result, 2, row(result, 1));
			result = row(result, 1, tmp);
		}
		result[0] *= scale_columns ?  1.f/scale :  1.f;
		result[1] *= scale_columns ? -1.f/scale : -1.f;
		result[2] *= scale_columns ? -1.f/scale : -1.f;
		result[3] = (result[3] - offset) / scale;
		return result;
	}

	mat4x3 OneD_TMat_to_ngp(const std::vector<float> &mat_array) {
		assert(mat_array.size() == 12);

		mat4x3 result = mat4x3::identity();

		for (int m = 0; m < 3; ++m) {
			for (int n = 0; n < 4; ++n) {
				if(n==3) result[n][m] = mat_array[n+m*4]*scale + offset[m];
				else result[n][m] = mat_array[n+m*4];
			}
		}
		return result;
	}

	std::vector<float> ngp_to_OneD_TMat(const mat4x3& nerf_matrix) {
		std::vector<float>  result(12,0.0f);

		for (int m = 0; m < 3; ++m) {
			for (int n = 0; n < 4; ++n) {
				if(n==3) result[n+m*4] = (nerf_matrix[n][m] - offset[m])/scale;
				result[n+m*4] = nerf_matrix[n][m];
			}
		}

		return result;
	}

	mat4x3 openGL_matrix_to_ngp(const mat4x3& nerf_matrix) const {
		mat4x3 result = nerf_matrix;
		result[3] = result[3] * scale + offset;

		return result;
	}

	vec3 openGL_point_to_ngp(const vec3& map_point) {
		vec3 result = map_point;

		result = result * scale + offset;
		return result;
	}

	vec3 ngp_position_to_nerf(vec3 pos) const {
		if (!from_mitsuba) {
			pos = vec3{pos.z, pos.x, pos.y};
		}
		return (pos - offset) / scale;
	}

	vec3 nerf_position_to_ngp(const vec3 &pos) const {
		vec3 rv = pos * scale + offset;
		return from_mitsuba ? rv : vec3{rv.y, rv.z, rv.x};
	}

	void nerf_ray_to_ngp(Ray& ray, bool scale_direction = false) {
		ray.o = ray.o * scale + offset;
		if (scale_direction) {
			ray.d *= scale;
		}

		float tmp = ray.o[0];
		ray.o[0] = ray.o[1];
		ray.o[1] = ray.o[2];
		ray.o[2] = tmp;

		tmp = ray.d[0];
		ray.d[0] = ray.d[1];
		ray.d[1] = ray.d[2];
		ray.d[2] = tmp;
	}
};

NerfDataset load_nerf(const std::vector<fs::path>& jsonpaths, float sharpen_amount = 0.f);
NerfDataset create_empty_nerf_dataset(size_t n_images, int aabb_scale = 1, bool is_hdr = false);

}
