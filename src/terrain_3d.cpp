// Copyright © 2025 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/height_map_shape3d.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include "geoclipmap.h"
#include "logger.h"
#include "terrain_3d.h"
#include "terrain_3d_util.h"

///////////////////////////
// Private Functions
///////////////////////////

// Initialize static member variable
int Terrain3D::debug_level{ ERROR };

void Terrain3D::_initialize() {
	LOG(INFO, "Checking initialization of main subsystems");

	// Make blank objects if needed
	if (_material.is_null()) {
		LOG(DEBUG, "Creating blank material");
		_material.instantiate();
	}
	if (!_data) {
		LOG(DEBUG, "Creating blank data object");
		_data = memnew(Terrain3DData);
	}
	if (_assets.is_null()) {
		LOG(DEBUG, "Creating blank texture list");
		_assets.instantiate();
	}
	if (!_collision) {
		LOG(DEBUG, "Creating collision manager");
		_collision = memnew(Terrain3DCollision);
	}
	if (!_instancer) {
		LOG(DEBUG, "Creating instancer");
		_instancer = memnew(Terrain3DInstancer);
	}

	// Connect signals
	// Any region was changed, update region labels
	if (!_data->is_connected("region_map_changed", callable_mp(this, &Terrain3D::update_region_labels))) {
		LOG(DEBUG, "Connecting _data::region_map_changed signal to set_show_region_locations()");
		_data->connect("region_map_changed", callable_mp(this, &Terrain3D::update_region_labels));
	}
	// Any region was changed, regenerate collision if enabled
	if (!_data->is_connected("region_map_changed", callable_mp(_collision, &Terrain3DCollision::build))) {
		LOG(DEBUG, "Connecting _data::region_map_changed signal to build()");
		_data->connect("region_map_changed", callable_mp(_collision, &Terrain3DCollision::build));
	}
	// Any map was regenerated or regions changed, update material
	if (!_data->is_connected("maps_changed", callable_mp(_material.ptr(), &Terrain3DMaterial::_update_maps))) {
		LOG(DEBUG, "Connecting _data::maps_changed signal to _material->_update_maps()");
		_data->connect("maps_changed", callable_mp(_material.ptr(), &Terrain3DMaterial::_update_maps));
	}
	// Height map was regenerated, update aabbs
	if (!_data->is_connected("height_maps_changed", callable_mp(this, &Terrain3D::update_aabbs))) {
		LOG(DEBUG, "Connecting _data::height_maps_changed signal to update_aabbs()");
		_data->connect("height_maps_changed", callable_mp(this, &Terrain3D::update_aabbs));
	}
	// Texture assets changed, update material
	if (!_assets->is_connected("textures_changed", callable_mp(_material.ptr(), &Terrain3DMaterial::_update_texture_arrays))) {
		LOG(DEBUG, "Connecting _assets.textures_changed to _material->_update_texture_arrays()");
		_assets->connect("textures_changed", callable_mp(_material.ptr(), &Terrain3DMaterial::_update_texture_arrays));
	}
	// MeshAssets changed, update instancer
	if (!_assets->is_connected("meshes_changed", callable_mp(_instancer, &Terrain3DInstancer::_update_mmis).bind(V2I_MAX, -1))) {
		LOG(DEBUG, "Connecting _assets.meshes_changed to _instancer->_update_mmis()");
		_assets->connect("meshes_changed", callable_mp(_instancer, &Terrain3DInstancer::_update_mmis).bind(V2I_MAX, -1));
	}

	// Initialize the system
	if (!_initialized && _is_inside_world && is_inside_tree()) {
		_data->initialize(this);
		_material->initialize(this);
		_assets->initialize(this);
		_collision->initialize(this);
		_instancer->initialize(this);
		_build_meshes(_mesh_lods, _mesh_size);
		_initialized = true;
	}
	update_configuration_warnings();
}

/**
 * This is a proxy for _process(delta) called by _notification() due to
 * https://github.com/godotengine/godot-cpp/issues/1022
 */
void Terrain3D::__physics_process(const double p_delta) {
	if (!_initialized)
		return;

	// If the game/editor camera is not set, find it
	if (!is_instance_valid(_camera_instance_id, _camera)) {
		LOG(DEBUG, "Camera is null, getting the current one");
		_grab_camera();
	}

	// If camera has moved enough, re-center the terrain on it.
	if (is_instance_valid(_camera_instance_id) && _camera->is_inside_tree()) {
		Vector3 cam_pos = _camera->get_global_position();
		Vector2 cam_pos_2d = Vector2(cam_pos.x, cam_pos.z);
		if (_camera_last_position.distance_to(cam_pos_2d) > 0.2f) {
			snap(cam_pos);
			_camera_last_position = cam_pos_2d;
		}
	}
}

/**
 * If running in the editor, grab the first editor viewport camera.
 * The edited_scene_root is excluded in case the user already has a Camera3D in their scene.
 */
void Terrain3D::_grab_camera() {
	if (IS_EDITOR) {
		_camera = EditorInterface::get_singleton()->get_editor_viewport_3d(0)->get_camera_3d();
		LOG(DEBUG, "Grabbing the first editor viewport camera: ", _camera);
	} else {
		_camera = get_viewport()->get_camera_3d();
		LOG(DEBUG, "Grabbing the in-game viewport camera: ", _camera);
	}
	if (_camera) {
		_camera_instance_id = _camera->get_instance_id();
	} else {
		_camera_instance_id = 0;
		set_physics_process(false); // disable snapping
		LOG(ERROR, "Cannot find the active camera. Set it manually with Terrain3D.set_camera(). Stopping _physics_process()");
	}
}

void Terrain3D::_build_containers() {
	_label_parent = memnew(Node3D);
	_label_parent->set_name("Labels");
	add_child(_label_parent, true);
	_mmi_parent = memnew(Node3D);
	_mmi_parent->set_name("MMI");
	add_child(_mmi_parent, true);
}

void Terrain3D::_destroy_containers() {
	memdelete_safely(_label_parent);
	memdelete_safely(_mmi_parent);
}

void Terrain3D::_destroy_labels() {
	Array labels = _label_parent->get_children();
	LOG(DEBUG, "Destroying ", labels.size(), " region labels");
	for (int i = 0; i < labels.size(); i++) {
		Node *label = cast_to<Node>(labels[i]);
		memdelete_safely(label);
	}
}

void Terrain3D::_destroy_instancer() {
	LOG(INFO, "Destroying Instancer");
	memdelete_safely(_instancer);
}

void Terrain3D::_destroy_collision(const bool p_final) {
	LOG(INFO, "Destroying Collision");
	if (_collision) {
		_collision->destroy();
	}
	if (p_final) {
		memdelete_safely(_collision);
	}
}

void Terrain3D::_build_meshes(const int p_mesh_lods, const int p_mesh_size) {
	if (!is_inside_tree() || !_data) {
		LOG(DEBUG, "Not inside the tree or no valid _data, skipping build");
		return;
	}
	LOG(INFO, "Building the terrain meshes");

	// Generate terrain meshes, lods, seams
	_meshes = GeoClipMap::generate(p_mesh_size, p_mesh_lods);
	ERR_FAIL_COND(_meshes.is_empty());

	// Set the current terrain material on all meshes
	RID material_rid = _material->get_material_rid();
	for (const RID rid : _meshes) {
		RS->mesh_surface_set_material(rid, 0, material_rid);
	}

	LOG(DEBUG, "Creating mesh instances");

	// Get current visual scenario so the instances appear in the scene
	RID scenario = get_world_3d()->get_scenario();

	bool baked_light;
	bool dynamic_gi;
	switch (_gi_mode) {
		case GeometryInstance3D::GI_MODE_DISABLED: {
			baked_light = false;
			dynamic_gi = false;
		} break;
		case GeometryInstance3D::GI_MODE_DYNAMIC: {
			baked_light = false;
			dynamic_gi = true;
		} break;
		case GeometryInstance3D::GI_MODE_STATIC:
		default: {
			baked_light = true;
			dynamic_gi = false;
		} break;
	}

	_mesh_data.cross = RS->instance_create2(_meshes[GeoClipMap::CROSS], scenario);
	RS->instance_set_layer_mask(_mesh_data.cross, _render_layers);
	RS->instance_geometry_set_cast_shadows_setting(_mesh_data.cross, _cast_shadows);
	RS->instance_geometry_set_flag(_mesh_data.cross, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
	RS->instance_geometry_set_flag(_mesh_data.cross, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);

	for (int lod = 0; lod < p_mesh_lods; lod++) {
		for (int x = 0; x < 4; x++) {
			for (int y = 0; y < 4; y++) {
				if (lod != 0 && (x == 1 || x == 2) && (y == 1 || y == 2)) {
					continue;
				}
				RID tile;
				if (lod == 0) {
					tile = RS->instance_create2(_meshes[GeoClipMap::TILE_INNER], scenario);
				} else {
					tile = RS->instance_create2(_meshes[GeoClipMap::TILE], scenario);
				}
				RS->instance_set_layer_mask(tile, _render_layers);
				RS->instance_geometry_set_cast_shadows_setting(tile, _cast_shadows);
				RS->instance_geometry_set_flag(tile, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
				RS->instance_geometry_set_flag(tile, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
				_mesh_data.tiles.push_back(tile);
			}
		}

		RID filler;
		if (lod == 0) {
			filler = RS->instance_create2(_meshes[GeoClipMap::FILLER_INNER], scenario);
		} else {
			filler = RS->instance_create2(_meshes[GeoClipMap::FILLER], scenario);
		}
		RS->instance_set_layer_mask(filler, _render_layers);
		RS->instance_geometry_set_cast_shadows_setting(filler, _cast_shadows);
		RS->instance_geometry_set_flag(filler, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
		RS->instance_geometry_set_flag(filler, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
		_mesh_data.fillers.push_back(filler);

		if (lod != p_mesh_lods - 1) {
			RID trim;
			if (lod == 0) {
				trim = RS->instance_create2(_meshes[GeoClipMap::TRIM_INNER], scenario);
			} else {
				trim = RS->instance_create2(_meshes[GeoClipMap::TRIM], scenario);
			}
			RS->instance_set_layer_mask(trim, _render_layers);
			RS->instance_geometry_set_cast_shadows_setting(trim, _cast_shadows);
			RS->instance_geometry_set_flag(trim, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
			RS->instance_geometry_set_flag(trim, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
			_mesh_data.trims.push_back(trim);

			RID seam = RS->instance_create2(_meshes[GeoClipMap::SEAM], scenario);
			RS->instance_set_layer_mask(seam, _render_layers);
			RS->instance_geometry_set_cast_shadows_setting(seam, _cast_shadows);
			RS->instance_geometry_set_flag(seam, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
			RS->instance_geometry_set_flag(seam, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
			_mesh_data.seams.push_back(seam);
		}
	}

	update_aabbs();
	// Force a snap update
	_camera_last_position = V2_MAX;
}

/**
 * Make all mesh instances visible or not
 * Update all mesh instances with the new world scenario so they appear
 */
void Terrain3D::_update_mesh_instances() {
	if (!_initialized || !_is_inside_world || !is_inside_tree()) {
		return;
	}

	RID _scenario = get_world_3d()->get_scenario();

	bool baked_light;
	bool dynamic_gi;
	switch (_gi_mode) {
		case GeometryInstance3D::GI_MODE_DISABLED: {
			baked_light = false;
			dynamic_gi = false;
		} break;
		case GeometryInstance3D::GI_MODE_DYNAMIC: {
			baked_light = false;
			dynamic_gi = true;
		} break;
		case GeometryInstance3D::GI_MODE_STATIC:
		default: {
			baked_light = true;
			dynamic_gi = false;
		} break;
	}

	bool v = is_visible_in_tree();
	RS->instance_set_visible(_mesh_data.cross, v);
	RS->instance_set_scenario(_mesh_data.cross, _scenario);
	RS->instance_set_layer_mask(_mesh_data.cross, _render_layers);
	RS->instance_geometry_set_cast_shadows_setting(_mesh_data.cross, _cast_shadows);
	RS->instance_geometry_set_flag(_mesh_data.cross, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
	RS->instance_geometry_set_flag(_mesh_data.cross, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);

	for (const RID rid : _mesh_data.tiles) {
		RS->instance_set_visible(rid, v);
		RS->instance_set_scenario(rid, _scenario);
		RS->instance_set_layer_mask(rid, _render_layers);
		RS->instance_geometry_set_cast_shadows_setting(rid, _cast_shadows);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
	}

	for (const RID rid : _mesh_data.fillers) {
		RS->instance_set_visible(rid, v);
		RS->instance_set_scenario(rid, _scenario);
		RS->instance_set_layer_mask(rid, _render_layers);
		RS->instance_geometry_set_cast_shadows_setting(rid, _cast_shadows);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
	}

	for (const RID rid : _mesh_data.trims) {
		RS->instance_set_visible(rid, v);
		RS->instance_set_scenario(rid, _scenario);
		RS->instance_set_layer_mask(rid, _render_layers);
		RS->instance_geometry_set_cast_shadows_setting(rid, _cast_shadows);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
	}

	for (const RID rid : _mesh_data.seams) {
		RS->instance_set_visible(rid, v);
		RS->instance_set_scenario(rid, _scenario);
		RS->instance_set_layer_mask(rid, _render_layers);
		RS->instance_geometry_set_cast_shadows_setting(rid, _cast_shadows);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_BAKED_LIGHT, baked_light);
		RS->instance_geometry_set_flag(rid, RenderingServer::INSTANCE_FLAG_USE_DYNAMIC_GI, dynamic_gi);
	}
}

void Terrain3D::_clear_meshes() {
	LOG(INFO, "Clearing the terrain meshes");
	for (const RID rid : _meshes) {
		RS->free_rid(rid);
	}
	RS->free_rid(_mesh_data.cross);
	for (const RID rid : _mesh_data.tiles) {
		RS->free_rid(rid);
	}
	for (const RID rid : _mesh_data.fillers) {
		RS->free_rid(rid);
	}
	for (const RID rid : _mesh_data.trims) {
		RS->free_rid(rid);
	}
	for (const RID rid : _mesh_data.seams) {
		RS->free_rid(rid);
	}
	_meshes.clear();
	_mesh_data.tiles.clear();
	_mesh_data.fillers.clear();
	_mesh_data.trims.clear();
	_mesh_data.seams.clear();
	_initialized = false;
}

void Terrain3D::_setup_mouse_picking() {
	if (!is_inside_tree()) {
		LOG(ERROR, "Not inside the tree, skipping mouse setup");
		return;
	}
	LOG(INFO, "Setting up mouse picker and get_intersection viewport, camera & screen quad");
	_mouse_vp = memnew(SubViewport);
	_mouse_vp->set_name("MouseViewport");
	add_child(_mouse_vp, true);
	_mouse_vp->set_size(Vector2i(2, 2));
	_mouse_vp->set_update_mode(SubViewport::UPDATE_ONCE);
	_mouse_vp->set_handle_input_locally(false);
	_mouse_vp->set_canvas_cull_mask(0);
	_mouse_vp->set_use_hdr_2d(true);
	_mouse_vp->set_default_canvas_item_texture_filter(Viewport::DEFAULT_CANVAS_ITEM_TEXTURE_FILTER_NEAREST);
	_mouse_vp->set_positional_shadow_atlas_size(0);
	_mouse_vp->set_positional_shadow_atlas_quadrant_subdiv(0, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED);
	_mouse_vp->set_positional_shadow_atlas_quadrant_subdiv(1, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED);
	_mouse_vp->set_positional_shadow_atlas_quadrant_subdiv(2, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED);
	_mouse_vp->set_positional_shadow_atlas_quadrant_subdiv(3, Viewport::SHADOW_ATLAS_QUADRANT_SUBDIV_DISABLED);

	_mouse_cam = memnew(Camera3D);
	_mouse_cam->set_name("MouseCamera");
	_mouse_vp->add_child(_mouse_cam, true);
	Ref<Environment> env;
	env.instantiate();
	env->set_tonemapper(Environment::TONE_MAPPER_LINEAR);
	_mouse_cam->set_environment(env);
	Ref<Compositor> comp;
	comp.instantiate();
	_mouse_cam->set_compositor(comp);
	_mouse_cam->set_projection(Camera3D::PROJECTION_ORTHOGONAL);
	_mouse_cam->set_size(0.1f);
	_mouse_cam->set_far(100000.f);

	_mouse_quad = memnew(MeshInstance3D);
	_mouse_quad->set_name("MouseQuad");
	_mouse_cam->add_child(_mouse_quad, true);
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(0.1f, 0.1f));
	_mouse_quad->set_mesh(quad);
	String shader_code = String(
#include "shaders/gpu_depth.glsl"
	);
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(shader_code);
	Ref<ShaderMaterial> shader_material;
	shader_material.instantiate();
	shader_material->set_shader(shader);
	shader_material->set_shader_parameter("compatibility", _compatibility);
	_mouse_quad->set_surface_override_material(0, shader_material);
	_mouse_quad->set_position(Vector3(0.f, 0.f, -0.5f));

	// Set terrain, terrain shader, mouse camera, and screen quad to mouse layer
	set_mouse_layer(_mouse_layer);
}

void Terrain3D::_destroy_mouse_picking() {
	LOG(DEBUG, "Freeing mouse_quad");
	memdelete_safely(_mouse_quad);
	LOG(DEBUG, "Freeing mouse_cam");
	memdelete_safely(_mouse_cam);
	LOG(DEBUG, "Freeing mouse_vp");
	memdelete_safely(_mouse_vp);
}

void Terrain3D::_generate_triangles(PackedVector3Array &p_vertices, PackedVector2Array *p_uvs, const int32_t p_lod,
		const Terrain3DData::HeightFilter p_filter, const bool p_require_nav, const AABB &p_global_aabb) const {
	ERR_FAIL_COND(_data == nullptr);
	int32_t step = 1 << CLAMP(p_lod, 0, 8);

	// Bake whole mesh, e.g. bake_mesh and painted navigation
	if (!p_global_aabb.has_volume()) {
		int32_t region_size = (int32_t)_region_size;

		TypedArray<Vector2i> region_locations = _data->get_region_locations();
		for (int r = 0; r < region_locations.size(); ++r) {
			Vector2i region_loc = (Vector2i)region_locations[r] * region_size;

			for (int32_t z = region_loc.y; z < region_loc.y + region_size; z += step) {
				for (int32_t x = region_loc.x; x < region_loc.x + region_size; x += step) {
					_generate_triangle_pair(p_vertices, p_uvs, p_lod, p_filter, p_require_nav, x, z);
				}
			}
		}
	} else {
		// Bake within an AABB, e.g. runtime navigation baker
		int32_t z_start = (int32_t)Math::ceil(p_global_aabb.position.z / _vertex_spacing);
		int32_t z_end = (int32_t)Math::floor(p_global_aabb.get_end().z / _vertex_spacing) + 1;
		int32_t x_start = (int32_t)Math::ceil(p_global_aabb.position.x / _vertex_spacing);
		int32_t x_end = (int32_t)Math::floor(p_global_aabb.get_end().x / _vertex_spacing) + 1;

		for (int32_t z = z_start; z < z_end; ++z) {
			for (int32_t x = x_start; x < x_end; ++x) {
				real_t height = _data->get_height(Vector3(x, 0.f, z));
				if (height >= p_global_aabb.position.y && height <= p_global_aabb.get_end().y) {
					_generate_triangle_pair(p_vertices, p_uvs, p_lod, p_filter, p_require_nav, x, z);
				}
			}
		}
	}
}

// Generates two triangles: Top 124, Bottom 143
//		1  __  2
//		  |\ |
//		  | \|
//		3  --  4
// p_vertices is assumed to exist and the destination for data
// p_uvs might not exist, so a pointer is fine
// p_require_nav is false for the runtime baker, which ignores navigation
void Terrain3D::_generate_triangle_pair(PackedVector3Array &p_vertices, PackedVector2Array *p_uvs,
		const int32_t p_lod, const Terrain3DData::HeightFilter p_filter, const bool p_require_nav,
		const int32_t x, const int32_t z) const {
	int32_t step = 1 << CLAMP(p_lod, 0, 8);
	Vector3 xz = Vector3(x, 0.0f, z) * _vertex_spacing;
	Vector3 xsz = Vector3(x + step, 0.0f, z) * _vertex_spacing;
	Vector3 xzs = Vector3(x, 0.0f, z + step) * _vertex_spacing;
	Vector3 xszs = Vector3(x + step, 0.0f, z + step) * _vertex_spacing;
	Vector3 v1 = _data->get_mesh_vertex(p_lod, p_filter, xz);
	bool nan1 = std::isnan(v1.y);
	if (nan1) {
		return;
	}
	Vector3 v2 = _data->get_mesh_vertex(p_lod, p_filter, xsz);
	Vector3 v3 = _data->get_mesh_vertex(p_lod, p_filter, xzs);
	Vector3 v4 = _data->get_mesh_vertex(p_lod, p_filter, xszs);
	bool nan2 = std::isnan(v2.y);
	bool nan3 = std::isnan(v3.y);
	bool nan4 = std::isnan(v4.y);
	// If on the region edge, duplicate the edge pixels
	// Check #2 upper right
	if (nan2) {
		v2.y = v1.y;
	}
	// Check #3 lower left
	if (nan3) {
		v3.y = v1.y;
	}
	// Check #4 lower right
	if (nan4) {
		if (!nan2) {
			v4.y = v2.y;
		} else if (!nan3) {
			v4.y = v3.y;
		} else {
			v4.y = v1.y;
		}
	}
	uint32_t ctrl1 = _data->get_control(xz);
	uint32_t ctrl2 = _data->get_control(xsz);
	uint32_t ctrl3 = _data->get_control(xzs);
	uint32_t ctrl4 = _data->get_control(xszs);
	// Holes are only where the control map is valid and the bit is set
	bool hole1 = ctrl1 != UINT32_MAX && is_hole(ctrl1);
	bool hole2 = ctrl2 != UINT32_MAX && is_hole(ctrl2);
	bool hole3 = ctrl3 != UINT32_MAX && is_hole(ctrl3);
	bool hole4 = ctrl4 != UINT32_MAX && is_hole(ctrl4);
	// Navigation is where the control map is valid and the bit is set, or it's the region edge and nav1 is set
	bool nav1 = ctrl1 != UINT32_MAX && is_nav(ctrl1);
	bool nav2 = ctrl2 != UINT32_MAX && is_nav(ctrl2) || nan2 && nav1;
	bool nav3 = ctrl3 != UINT32_MAX && is_nav(ctrl3) || nan3 && nav1;
	bool nav4 = ctrl4 != UINT32_MAX && is_nav(ctrl4) || nan4 && nav1;
	//Bottom 143 triangle
	if (!(hole1 || hole4 || hole3) && (!p_require_nav || (nav1 && nav4 && nav3))) {
		p_vertices.push_back(v1);
		p_vertices.push_back(v4);
		p_vertices.push_back(v3);
		if (p_uvs) {
			p_uvs->push_back(Vector2(v1.x, v1.z));
			p_uvs->push_back(Vector2(v4.x, v4.z));
			p_uvs->push_back(Vector2(v3.x, v3.z));
		}
	}
	// Top 124 triangle
	if (!(hole1 || hole2 || hole4) && (!p_require_nav || (nav1 && nav2 && nav4))) {
		p_vertices.push_back(v1);
		p_vertices.push_back(v2);
		p_vertices.push_back(v4);
		if (p_uvs) {
			p_uvs->push_back(Vector2(v1.x, v1.z));
			p_uvs->push_back(Vector2(v2.x, v2.z));
			p_uvs->push_back(Vector2(v4.x, v4.z));
		}
	}
}

///////////////////////////
// Public Functions
///////////////////////////

Terrain3D::Terrain3D() {
	// Check if we are using the compatibility renderer
	_compatibility = String(ProjectSettings::get_singleton()->get_setting_with_override("rendering/renderer/rendering_method")).contains("gl_compatibility");

	// Process the command line
	PackedStringArray args = OS::get_singleton()->get_cmdline_args();
	for (int i = args.size() - 1; i >= 0; i--) {
		String arg = args[i];
		if (arg.begins_with("--terrain3d-debug=")) {
			String value = arg.rsplit("=")[1];
			if (value == "ERROR") {
				set_debug_level(ERROR);
			} else if (value == "INFO") {
				set_debug_level(INFO);
			} else if (value == "DEBUG") {
				set_debug_level(DEBUG);
			} else if (value == "EXTREME") {
				set_debug_level(EXTREME);
			}
		} else if (arg.begins_with("--terrain3d-renderer=")) {
			String value = arg.rsplit("=")[1];
			if (value == "compatibility") {
				_compatibility = true;
			}
		}
	}
}

void Terrain3D::set_debug_level(const int p_level) {
	LOG(INFO, "Setting debug level: ", p_level);
	debug_level = CLAMP(p_level, 0, EXTREME);
}

void Terrain3D::set_data_directory(String p_dir) {
	LOG(INFO, "Setting data directory to ", p_dir);
	if (_data_directory != p_dir) {
		_clear_meshes();
		_destroy_labels();
		_destroy_collision();
		_destroy_instancer();
		memdelete_safely(_data);
		_data_directory = p_dir;
		_initialize();
	}
	update_configuration_warnings();
}

void Terrain3D::set_material(const Ref<Terrain3DMaterial> &p_material) {
	if (_material != p_material) {
		_clear_meshes();
		LOG(INFO, "Setting material");
		_material = p_material;
		_initialize();
		emit_signal("material_changed");
	}
}

void Terrain3D::set_assets(const Ref<Terrain3DAssets> &p_assets) {
	if (_assets != p_assets) {
		_clear_meshes();
		LOG(INFO, "Setting asset list");
		_assets = p_assets;
		_initialize();
		emit_signal("assets_changed");
	}
}

void Terrain3D::set_editor(Terrain3DEditor *p_editor) {
	_editor = p_editor;
	_material->update();
	LOG(DEBUG, "Received Terrain3DEditor: ", p_editor);
}

void Terrain3D::set_plugin(EditorPlugin *p_plugin) {
	_plugin = p_plugin;
	LOG(DEBUG, "Received editor plugin: ", p_plugin);
}

void Terrain3D::set_camera(Camera3D *p_camera) {
	if (_camera != p_camera) {
		_camera = p_camera;
		if (!p_camera) {
			LOG(DEBUG, "Received null camera. Calling _grab_camera()");
			_grab_camera();
		} else {
			_camera = p_camera;
			_camera_instance_id = _camera->get_instance_id();
			LOG(DEBUG, "Setting camera: ", _camera);
			_initialize();
			set_physics_process(true); // enable snapping
		}
	}
}

void Terrain3D::set_region_size(const RegionSize p_size) {
	LOG(INFO, "Setting region size: ", p_size);
	ERR_FAIL_COND(p_size < SIZE_64);
	ERR_FAIL_COND(p_size > SIZE_2048);
	_region_size = p_size;
	if (_data) {
		_data->_region_size = _region_size;
		_data->_region_sizev = Vector2i(_region_size, _region_size);
	}
	if (_material.is_valid()) {
		_material->_update_maps();
	}
}

void Terrain3D::set_save_16_bit(const bool p_enabled) {
	LOG(INFO, p_enabled);
	_save_16_bit = p_enabled;
}

void Terrain3D::set_label_distance(const real_t p_distance) {
	real_t distance = CLAMP(p_distance, 0.f, 100000.f);
	LOG(INFO, "Setting region label distance: ", distance);
	if (_label_distance != distance) {
		_label_distance = distance;
		update_region_labels();
	}
}

void Terrain3D::set_label_size(const int p_size) {
	int size = CLAMP(p_size, 24, 128);
	LOG(INFO, "Setting region label size: ", size);
	if (_label_size != size) {
		_label_size = size;
		update_region_labels();
	}
}

void Terrain3D::update_region_labels() {
	_destroy_labels();
	if (_label_distance > 0.f && _data) {
		Array region_locations = _data->get_region_locations();
		LOG(DEBUG, "Creating ", region_locations.size(), " region labels");
		for (int i = 0; i < region_locations.size(); i++) {
			Label3D *label = memnew(Label3D);
			String text = region_locations[i];
			label->set_name("Label3D" + text.replace(" ", ""));
			label->set_pixel_size(.001f);
			label->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
			label->set_draw_flag(Label3D::FLAG_DOUBLE_SIDED, true);
			label->set_draw_flag(Label3D::FLAG_DISABLE_DEPTH_TEST, true);
			label->set_draw_flag(Label3D::FLAG_FIXED_SIZE, true);
			label->set_text(text);
			label->set_modulate(Color(1.f, 1.f, 1.f, (_compatibility) ? .9f : .5f));
			label->set_outline_modulate(Color(0.f, 0.f, 0.f, .5f));
			label->set_font_size(_label_size);
			label->set_outline_size(_label_size / 6);
			label->set_visibility_range_end(_label_distance);
			label->set_visibility_range_end_margin(_label_distance / 10.f);
			label->set_visibility_range_fade_mode(GeometryInstance3D::VISIBILITY_RANGE_FADE_SELF);
			_label_parent->add_child(label, true);
			Vector2i loc = region_locations[i];
			Vector3 pos = Vector3(real_t(loc.x) + .5f, 0.f, real_t(loc.y) + .5f) * _region_size * _vertex_spacing;
			real_t height = _data->get_height(pos);
			pos.y = (std::isnan(height)) ? 0 : height;
			label->set_position(pos);
		}
	}
}

void Terrain3D::set_mesh_lods(const int p_count) {
	if (_mesh_lods != p_count) {
		_clear_meshes();
		_destroy_collision();
		LOG(INFO, "Setting mesh levels: ", p_count);
		_mesh_lods = p_count;
		_initialize();
	}
}

void Terrain3D::set_mesh_size(const int p_size) {
	if (_mesh_size != p_size) {
		_clear_meshes();
		_destroy_collision();
		LOG(INFO, "Setting mesh size: ", p_size);
		_mesh_size = p_size;
		_initialize();
	}
}

void Terrain3D::set_vertex_spacing(const real_t p_spacing) {
	real_t spacing = CLAMP(p_spacing, 0.25f, 100.0f);
	if (_vertex_spacing != spacing) {
		_vertex_spacing = spacing;
		LOG(INFO, "Setting vertex spacing: ", _vertex_spacing);
		_clear_meshes();
		_destroy_collision();
		_destroy_instancer();
		_initialize();
		_data->_vertex_spacing = _vertex_spacing;
		update_region_labels();
		_instancer->_update_vertex_spacing(_vertex_spacing);
	}
	if (IS_EDITOR && _plugin) {
		_plugin->call("update_region_grid");
	}
}

void Terrain3D::set_render_layers(const uint32_t p_layers) {
	LOG(INFO, "Setting terrain render layers to: ", p_layers);
	_render_layers = p_layers;
	_update_mesh_instances();
}

void Terrain3D::set_mouse_layer(const uint32_t p_layer) {
	uint32_t layer = CLAMP(p_layer, 21, 32);
	_mouse_layer = layer;
	uint32_t mouse_mask = 1 << (_mouse_layer - 1);
	LOG(INFO, "Setting mouse layer: ", layer, " (", mouse_mask, ") on terrain mesh, material, mouse camera, mouse quad");

	// Set terrain meshes to mouse layer
	// Mask off editor render layers by ORing user layers 1-20 and current mouse layer
	set_render_layers((_render_layers & 0xFFFFF) | mouse_mask);
	// Set terrain shader to exclude mouse camera from showing holes
	if (_material.is_valid()) {
		_material->set_shader_param("_mouse_layer", mouse_mask);
	}
	// Set mouse camera to see only mouse layer
	if (_mouse_cam) {
		_mouse_cam->set_cull_mask(mouse_mask);
	}
	// Set screenquad to mouse layer
	if (_mouse_quad) {
		_mouse_quad->set_layer_mask(mouse_mask);
	}
}

void Terrain3D::set_cast_shadows(const RenderingServer::ShadowCastingSetting p_cast_shadows) {
	_cast_shadows = p_cast_shadows;
	_update_mesh_instances();
}

void Terrain3D::set_gi_mode(const GeometryInstance3D::GIMode p_gi_mode) {
	_gi_mode = p_gi_mode;
	_update_mesh_instances();
}

void Terrain3D::set_cull_margin(const real_t p_margin) {
	LOG(INFO, "Setting extra cull margin: ", p_margin);
	_cull_margin = p_margin;
	update_aabbs();
}

/**
 * Centers the terrain and LODs on a provided position. Y height is ignored.
 */
void Terrain3D::snap(const Vector3 &p_cam_pos) {
	Vector3 cam_pos = p_cam_pos;
	cam_pos.y = 0.f;
	_snapped_position = (cam_pos / _vertex_spacing).floor() * _vertex_spacing;
	LOG(EXTREME, "Snapping terrain to: ", _snapped_position);

	Transform3D scaled_t = Transform3D().scaled(Vector3(_vertex_spacing, 1.f, _vertex_spacing));
	scaled_t.origin = _snapped_position;
	RS->instance_set_transform(_mesh_data.cross, scaled_t);
	RS->instance_reset_physics_interpolation(_mesh_data.cross);

	int edge = 0;
	int tile = 0;

	for (int l = 0; l < _mesh_lods; l++) {
		real_t scale = real_t(1 << l) * _vertex_spacing;
		Vector3 snapped_pos = (cam_pos / scale).floor() * scale;
		Vector3 tile_size = Vector3(real_t(_mesh_size << l), 0, real_t(_mesh_size << l)) * _vertex_spacing;
		Vector3 base = snapped_pos - Vector3(real_t(_mesh_size << (l + 1)), 0.f, real_t(_mesh_size << (l + 1))) * _vertex_spacing;

		// Position tiles
		for (int x = 0; x < 4; x++) {
			for (int y = 0; y < 4; y++) {
				if (l != 0 && (x == 1 || x == 2) && (y == 1 || y == 2)) {
					continue;
				}

				Vector3 fill = Vector3(x >= 2 ? 1.f : 0.f, 0.f, y >= 2 ? 1.f : 0.f) * scale;
				Vector3 tile_tl = base + Vector3(x, 0.f, y) * tile_size + fill;
				//Vector3 tile_br = tile_tl + tile_size;

				Transform3D t = Transform3D().scaled(Vector3(scale, 1.f, scale));
				t.origin = tile_tl;

				RS->instance_set_transform(_mesh_data.tiles[tile], t);
				RS->instance_reset_physics_interpolation(_mesh_data.tiles[tile]);

				tile++;
			}
		}
		{
			Transform3D t = Transform3D().scaled(Vector3(scale, 1.f, scale));
			t.origin = snapped_pos;
			RS->instance_set_transform(_mesh_data.fillers[l], t);
			RS->instance_reset_physics_interpolation(_mesh_data.fillers[l]);
		}

		if (l != _mesh_lods - 1) {
			real_t next_scale = scale * 2.f;
			Vector3 next_snapped_pos = (cam_pos / next_scale).floor() * next_scale;

			// Position trims
			{
				Vector3 tile_center = snapped_pos + (Vector3(scale, 0.f, scale) * 0.5f);
				Vector3 d = cam_pos - next_snapped_pos;

				int r = 0;
				r |= d.x >= scale ? 0 : 2;
				r |= d.z >= scale ? 0 : 1;

				real_t rotations[4] = { 0.f, 270.f, 90.f, 180.f };

				real_t angle = UtilityFunctions::deg_to_rad(rotations[r]);
				Transform3D t = Transform3D().rotated(Vector3(0.f, 1.f, 0.f), -angle);
				t = t.scaled(Vector3(scale, 1.f, scale));
				t.origin = tile_center;
				RS->instance_set_transform(_mesh_data.trims[edge], t);
				RS->instance_reset_physics_interpolation(_mesh_data.trims[edge]);
			}

			// Position seams
			{
				Vector3 next_base = next_snapped_pos - Vector3(real_t(_mesh_size << (l + 1)), 0.f, real_t(_mesh_size << (l + 1))) * _vertex_spacing;
				Transform3D t = Transform3D().scaled(Vector3(scale, 1.f, scale));
				t.origin = next_base;
				RS->instance_set_transform(_mesh_data.seams[edge], t);
				RS->instance_reset_physics_interpolation(_mesh_data.seams[edge]);
			}
			edge++;
		}
	}

	if (_collision && _collision->is_dynamic_mode()) {
		_collision->update();
	}
}

void Terrain3D::update_aabbs() {
	if (_meshes.is_empty() || !_data) {
		LOG(DEBUG, "Update AABB called before terrain meshes built. Returning.");
		return;
	}

	Vector2 height_range = _data->get_height_range();
	LOG(EXTREME, "Updating AABBs. Total height range: ", height_range, ", extra cull margin: ", _cull_margin);
	height_range.y += abs(height_range.x); // Add below zero to total size

	AABB aabb = RS->mesh_get_custom_aabb(_meshes[GeoClipMap::CROSS]);
	aabb.position.y = height_range.x - _cull_margin;
	aabb.size.y = height_range.y + _cull_margin * 2.f;
	RS->instance_set_custom_aabb(_mesh_data.cross, aabb);

	aabb = RS->mesh_get_custom_aabb(_meshes[GeoClipMap::TILE]);
	aabb.position.y = height_range.x - _cull_margin;
	aabb.size.y = height_range.y + _cull_margin * 2.f;
	for (int i = 0; i < _mesh_data.tiles.size(); i++) {
		RS->instance_set_custom_aabb(_mesh_data.tiles[i], aabb);
	}

	aabb = RS->mesh_get_custom_aabb(_meshes[GeoClipMap::FILLER]);
	aabb.position.y = height_range.x - _cull_margin;
	aabb.size.y = height_range.y + _cull_margin * 2.f;
	for (int i = 0; i < _mesh_data.fillers.size(); i++) {
		RS->instance_set_custom_aabb(_mesh_data.fillers[i], aabb);
	}

	aabb = RS->mesh_get_custom_aabb(_meshes[GeoClipMap::TRIM]);
	aabb.position.y = height_range.x - _cull_margin;
	aabb.size.y = height_range.y + _cull_margin * 2.f;
	for (int i = 0; i < _mesh_data.trims.size(); i++) {
		RS->instance_set_custom_aabb(_mesh_data.trims[i], aabb);
	}

	aabb = RS->mesh_get_custom_aabb(_meshes[GeoClipMap::SEAM]);
	aabb.position.y = height_range.x - _cull_margin;
	aabb.size.y = height_range.y + _cull_margin * 2.f;
	for (int i = 0; i < _mesh_data.seams.size(); i++) {
		RS->instance_set_custom_aabb(_mesh_data.seams[i], aabb);
	}
}

/* Returns the point a ray intersects the ground using either raymarching or the GPU depth texture
 *	p_src_pos (camera position)
 *	p_direction (camera direction looking at the terrain)
 *  p_gpu_mode - false: use raymarching, true: use GPU mode
 * Returns Vec3(NAN) on error or vec3(3.402823466e+38F) on no intersection. Test w/ if (var.x < 3.4e38)
 */
Vector3 Terrain3D::get_intersection(const Vector3 &p_src_pos, const Vector3 &p_direction, const bool p_gpu_mode) {
	if (!is_instance_valid(_camera_instance_id)) {
		LOG(ERROR, "Invalid camera");
		return Vector3(NAN, NAN, NAN);
	}
	if (!_mouse_cam) {
		LOG(ERROR, "Invalid mouse camera");
		return Vector3(NAN, NAN, NAN);
	}
	Vector3 direction = p_direction.normalized();
	Vector3 point;

	// Position mouse cam one unit behind the requested position
	_mouse_cam->set_global_position(p_src_pos - direction);

	// If looking straight down (eg orthogonal camera), just return height. look_at won't work
	if ((direction - Vector3(0.f, -1.f, 0.f)).length_squared() < 0.00001f) {
		_mouse_cam->set_rotation_degrees(Vector3(-90.f, 0.f, 0.f));
		point = p_src_pos;
		point.y = _data->get_height(p_src_pos);
		if (std::isnan(point.y)) {
			point.y = 0;
		}

	} else if (!p_gpu_mode) {
		// Else if not gpu mode, use raymarching mode
		point = p_src_pos;
		for (int i = 0; i < 4000; i++) {
			real_t height = _data->get_height(point);
			if (point.y - height <= 0) {
				return point;
			}
			point += direction;
		}
		return V3_MAX;

	} else {
		// Else use GPU mode, which requires multiple calls
		// Get depth from perspective camera snapshot
		_mouse_cam->look_at(_mouse_cam->get_global_position() + direction, Vector3(0.f, 1.f, 0.f));
		_mouse_vp->set_update_mode(SubViewport::UPDATE_ONCE);
		Ref<ViewportTexture> vp_tex = _mouse_vp->get_texture();
		Ref<Image> vp_img = vp_tex->get_image();

		// Read the depth pixel from the camera viewport
		Color screen_depth = vp_img->get_pixel(0, 0);

		// Get position from depth packed in RGB - unpack back to float.
		// Forward+ is 16bit, mobile is 10bit and compatibility is 8bit.
		// Compatibility also has precision loss for values below 0.5, so
		// we use only the top half of the range, for 21bit depth encoded.
		real_t r = floor((screen_depth.r * 256.0) - 128.0);
		real_t g = floor((screen_depth.g * 256.0) - 128.0);
		real_t b = floor((screen_depth.b * 256.0) - 128.0);

		// Decode the full depth value
		real_t decoded_depth = (r + g / 127.0 + b / (127.0 * 127.0)) / 127.0;

		if (decoded_depth < 0.00001f) {
			return V3_MAX;
		}
		// Necessary for a correct value depth = 1
		if (decoded_depth > 0.99999f) {
			decoded_depth = 1.0f;
		}

		// Denormalize distance to get real depth and terrain position.
		decoded_depth *= _mouse_cam->get_far();

		// Project the camera position by the depth value to get the intersection point.
		point = _mouse_cam->get_global_position() + direction * decoded_depth;
	}

	return point;
}

/**
 * Generates a static ArrayMesh for the terrain.
 * p_lod (0-8): Determines the granularity of the generated mesh.
 * p_filter: Controls how vertices' Y coordinates are generated from the height map.
 *  HEIGHT_FILTER_NEAREST: Samples the height map in a 'nearest neighbour' fashion.
 *  HEIGHT_FILTER_MINIMUM: Samples a range of heights around each vertex and returns the lowest.
 *   This takes longer than ..._NEAREST, but can be used to create occluders, since it can guarantee the
 *   generated mesh will not extend above or outside the clipmap at any LOD.
 */
Ref<Mesh> Terrain3D::bake_mesh(const int p_lod, const Terrain3DData::HeightFilter p_filter) const {
	LOG(INFO, "Baking mesh at lod: ", p_lod, " with filter: ", p_filter);
	Ref<Mesh> result;
	ERR_FAIL_COND_V(_data == nullptr, result);

	Ref<SurfaceTool> st;
	st.instantiate();
	st->begin(Mesh::PRIMITIVE_TRIANGLES);

	PackedVector3Array vertices;
	PackedVector2Array uvs;
	_generate_triangles(vertices, &uvs, p_lod, p_filter, false, AABB());

	ERR_FAIL_COND_V(vertices.size() != uvs.size(), result);
	for (int i = 0; i < vertices.size(); ++i) {
		st->set_uv(uvs[i]);
		st->add_vertex(vertices[i]);
	}

	st->index();
	st->generate_normals();
	st->generate_tangents();
	st->optimize_indices_for_cache();
	result = st->commit();
	return result;
}

/**
 * Generates source geometry faces for input to nav mesh baking. Geometry is only generated where there
 * are no holes and the terrain has been painted as navigable.
 * p_global_aabb: If non-empty, geometry will be generated only within this AABB. If empty, geometry
 *  will be generated for the entire terrain.
 * p_require_nav: If true, this function will only generate geometry for terrain marked navigable.
 *  Otherwise, geometry is generated for the entire terrain within the AABB (which can be useful for
 *  dynamic and/or runtime nav mesh baking).
 */
PackedVector3Array Terrain3D::generate_nav_mesh_source_geometry(const AABB &p_global_aabb, const bool p_require_nav) const {
	LOG(INFO, "Generating NavMesh source geometry from terrain");
	PackedVector3Array faces;
	_generate_triangles(faces, nullptr, 0, Terrain3DData::HEIGHT_FILTER_NEAREST, p_require_nav, p_global_aabb);
	return faces;
}

void Terrain3D::set_warning(const uint8_t p_warning, const bool p_enabled) {
	if (p_enabled) {
		_warnings |= p_warning;
	} else {
		_warnings &= ~p_warning;
	}
	update_configuration_warnings();
}

PackedStringArray Terrain3D::_get_configuration_warnings() const {
	PackedStringArray psa;
	if (_data_directory.is_empty()) {
		psa.push_back("No data directory specified. Select a directory then save the scene to write data.");
	}
	if (_warnings & WARN_MISMATCHED_SIZE) {
		psa.push_back("Texture dimensions don't match. Double-click a texture in the FileSystem panel to see its size. Read Texture Prep in docs.");
	}
	if (_warnings & WARN_MISMATCHED_FORMAT) {
		psa.push_back("Texture formats don't match. Double-click a texture in the FileSystem panel to see its format. Check Import panel. Read Texture Prep in docs.");
	}
	if (_warnings & WARN_MISMATCHED_MIPMAPS) {
		psa.push_back("Texture mipmap settings don't match. Change on the Import panel.");
	}
	return psa;
}

///////////////////////////
// Protected Functions
///////////////////////////

// Notifications are defined in individual classes: Object, Node, Node3D
// Listed below in order of operation
void Terrain3D::_notification(const int p_what) {
	switch (p_what) {
			/// Startup notifications

		case NOTIFICATION_POSTINITIALIZE: {
			// Object initialized, before script is attached
			LOG(INFO, "NOTIFICATION_POSTINITIALIZE");
			_build_containers();
			break;
		}

		case NOTIFICATION_ENTER_WORLD: {
			// Node3D registered to new World3D resource
			// Sent on scene changes
			LOG(INFO, "NOTIFICATION_ENTER_WORLD");
			_is_inside_world = true;
			_update_mesh_instances();
			break;
		}

		case NOTIFICATION_ENTER_TREE: {
			// Node entered a SceneTree
			// Sent on scene changes
			LOG(INFO, "NOTIFICATION_ENTER_TREE");
			set_as_top_level(true); // Don't inherit transforms from parent. Global only.
			set_notify_transform(true);
			set_meta("_edit_lock_", true);
			_setup_mouse_picking();
			_initialize(); // Rebuild anything freed: meshes, collision, instancer
			set_physics_process(true);
			break;
		}

		case NOTIFICATION_READY: {
			// Node is ready
			LOG(INFO, "NOTIFICATION_READY");
			break;
		}

			/// Game Loop notifications

		case NOTIFICATION_PHYSICS_PROCESS: {
			// Node is processing one physics frame
			__physics_process(get_process_delta_time());
			break;
		}

		case NOTIFICATION_TRANSFORM_CHANGED: {
			// Node3D or parent transform changed
			if (get_transform() != Transform3D()) {
				set_transform(Transform3D());
			}
			break;
		}

		case NOTIFICATION_VISIBILITY_CHANGED: {
			// Node3D visibility changed
			LOG(INFO, "NOTIFICATION_VISIBILITY_CHANGED");
			_update_mesh_instances();
			break;
		}

		case NOTIFICATION_EXTENSION_RELOADED: {
			// Object finished hot reloading
			LOG(INFO, "NOTIFICATION_EXTENSION_RELOADED");
			break;
		}

		case NOTIFICATION_EDITOR_PRE_SAVE: {
			// Editor Node is about to save the current scene
			LOG(INFO, "NOTIFICATION_EDITOR_PRE_SAVE");
			if (_data_directory.is_empty()) {
				LOG(ERROR, "Data directory is empty. Set it to write data to disk.");
			} else if (!_data) {
				LOG(DEBUG, "Save requested, but no valid data object. Skipping");
			} else {
				_data->save_directory(_data_directory);
			}
			if (!_material.is_valid()) {
				LOG(DEBUG, "Save requested, but no valid material. Skipping");
			} else {
				_material->save();
			}
			if (!_assets.is_valid()) {
				LOG(DEBUG, "Save requested, but no valid texture list. Skipping");
			} else {
				_assets->save();
			}
			break;
		}

		case NOTIFICATION_EDITOR_POST_SAVE: {
			// Editor Node finished saving current scene
			break;
		}

		case NOTIFICATION_CRASH: {
			// Godot's crash handler reports engine is about to crash
			// Only on desktop if the crash handler is enabled
			LOG(WARN, "NOTIFICATION_CRASH");
			break;
		}

			/// Shut down notifications

		case NOTIFICATION_EXIT_TREE: {
			// Node is about to exit a SceneTree
			// Sent on scene changes
			LOG(INFO, "NOTIFICATION_EXIT_TREE");
			set_physics_process(false);
			_clear_meshes();
			_destroy_mouse_picking();
			break;
		}

		case NOTIFICATION_EXIT_WORLD: {
			// Node3D unregistered from current World3D
			// Sent on scene changes
			LOG(INFO, "NOTIFICATION_EXIT_WORLD");
			_is_inside_world = false;
			break;
		}

		case NOTIFICATION_PREDELETE: {
			// Object is about to be deleted
			LOG(INFO, "NOTIFICATION_PREDELETE");
			_destroy_collision(true);
			_destroy_instancer();
			_destroy_labels();
			_destroy_containers();
			memdelete_safely(_data);
			break;
		}

		default:
			break;
	}
}

void Terrain3D::_bind_methods() {
	BIND_ENUM_CONSTANT(SIZE_64);
	BIND_ENUM_CONSTANT(SIZE_128);
	BIND_ENUM_CONSTANT(SIZE_256);
	BIND_ENUM_CONSTANT(SIZE_512);
	BIND_ENUM_CONSTANT(SIZE_1024);
	BIND_ENUM_CONSTANT(SIZE_2048);

	ClassDB::bind_method(D_METHOD("get_version"), &Terrain3D::get_version);
	ClassDB::bind_method(D_METHOD("set_debug_level", "level"), &Terrain3D::set_debug_level);
	ClassDB::bind_method(D_METHOD("get_debug_level"), &Terrain3D::get_debug_level);
	ClassDB::bind_method(D_METHOD("set_data_directory", "directory"), &Terrain3D::set_data_directory);
	ClassDB::bind_method(D_METHOD("get_data_directory"), &Terrain3D::get_data_directory);

	// Object references
	ClassDB::bind_method(D_METHOD("get_data"), &Terrain3D::get_data);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &Terrain3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &Terrain3D::get_material);
	ClassDB::bind_method(D_METHOD("set_assets", "assets"), &Terrain3D::set_assets);
	ClassDB::bind_method(D_METHOD("get_assets"), &Terrain3D::get_assets);
	ClassDB::bind_method(D_METHOD("get_collision"), &Terrain3D::get_collision);
	ClassDB::bind_method(D_METHOD("get_instancer"), &Terrain3D::get_instancer);
	ClassDB::bind_method(D_METHOD("set_editor", "editor"), &Terrain3D::set_editor);
	ClassDB::bind_method(D_METHOD("get_editor"), &Terrain3D::get_editor);
	ClassDB::bind_method(D_METHOD("set_plugin", "plugin"), &Terrain3D::set_plugin);
	ClassDB::bind_method(D_METHOD("get_plugin"), &Terrain3D::get_plugin);
	ClassDB::bind_method(D_METHOD("set_camera", "camera"), &Terrain3D::set_camera);
	ClassDB::bind_method(D_METHOD("get_camera"), &Terrain3D::get_camera);

	//Regions
	ClassDB::bind_method(D_METHOD("change_region_size", "size"), &Terrain3D::change_region_size);
	ClassDB::bind_method(D_METHOD("get_region_size"), &Terrain3D::get_region_size);
	ClassDB::bind_method(D_METHOD("set_save_16_bit", "enabled"), &Terrain3D::set_save_16_bit);
	ClassDB::bind_method(D_METHOD("get_save_16_bit"), &Terrain3D::get_save_16_bit);
	ClassDB::bind_method(D_METHOD("set_label_distance", "distance"), &Terrain3D::set_label_distance);
	ClassDB::bind_method(D_METHOD("get_label_distance"), &Terrain3D::get_label_distance);
	ClassDB::bind_method(D_METHOD("set_label_size", "size"), &Terrain3D::set_label_size);
	ClassDB::bind_method(D_METHOD("get_label_size"), &Terrain3D::get_label_size);

	// Collision
	ClassDB::bind_method(D_METHOD("set_collision_mode", "mode"), &Terrain3D::set_collision_mode);
	ClassDB::bind_method(D_METHOD("get_collision_mode"), &Terrain3D::get_collision_mode);
	ClassDB::bind_method(D_METHOD("set_collision_shape_size", "size"), &Terrain3D::set_collision_shape_size);
	ClassDB::bind_method(D_METHOD("get_collision_shape_size"), &Terrain3D::get_collision_shape_size);
	ClassDB::bind_method(D_METHOD("set_collision_radius", "radius"), &Terrain3D::set_collision_radius);
	ClassDB::bind_method(D_METHOD("get_collision_radius"), &Terrain3D::get_collision_radius);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layers"), &Terrain3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Terrain3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Terrain3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Terrain3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_priority", "priority"), &Terrain3D::set_collision_priority);
	ClassDB::bind_method(D_METHOD("get_collision_priority"), &Terrain3D::get_collision_priority);
	ClassDB::bind_method(D_METHOD("set_physics_material", "material"), &Terrain3D::set_physics_material);
	ClassDB::bind_method(D_METHOD("get_physics_material"), &Terrain3D::get_physics_material);

	// Meshes
	ClassDB::bind_method(D_METHOD("set_mesh_lods", "count"), &Terrain3D::set_mesh_lods);
	ClassDB::bind_method(D_METHOD("get_mesh_lods"), &Terrain3D::get_mesh_lods);
	ClassDB::bind_method(D_METHOD("set_mesh_size", "size"), &Terrain3D::set_mesh_size);
	ClassDB::bind_method(D_METHOD("get_mesh_size"), &Terrain3D::get_mesh_size);
	ClassDB::bind_method(D_METHOD("set_vertex_spacing", "scale"), &Terrain3D::set_vertex_spacing);
	ClassDB::bind_method(D_METHOD("get_vertex_spacing"), &Terrain3D::get_vertex_spacing);
	ClassDB::bind_method(D_METHOD("get_snapped_position"), &Terrain3D::get_snapped_position);

	// Rendering
	ClassDB::bind_method(D_METHOD("set_render_layers", "layers"), &Terrain3D::set_render_layers);
	ClassDB::bind_method(D_METHOD("get_render_layers"), &Terrain3D::get_render_layers);
	ClassDB::bind_method(D_METHOD("set_mouse_layer", "layer"), &Terrain3D::set_mouse_layer);
	ClassDB::bind_method(D_METHOD("get_mouse_layer"), &Terrain3D::get_mouse_layer);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "shadow_casting_setting"), &Terrain3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("get_cast_shadows"), &Terrain3D::get_cast_shadows);
	ClassDB::bind_method(D_METHOD("set_gi_mode", "gi_mode"), &Terrain3D::set_gi_mode);
	ClassDB::bind_method(D_METHOD("get_gi_mode"), &Terrain3D::get_gi_mode);
	ClassDB::bind_method(D_METHOD("set_cull_margin", "margin"), &Terrain3D::set_cull_margin);
	ClassDB::bind_method(D_METHOD("get_cull_margin"), &Terrain3D::get_cull_margin);
	ClassDB::bind_method(D_METHOD("set_show_instances", "visible"), &Terrain3D::set_show_instances);
	ClassDB::bind_method(D_METHOD("get_show_instances"), &Terrain3D::get_show_instances);
	ClassDB::bind_method(D_METHOD("is_compatibility_mode"), &Terrain3D::is_compatibility_mode);

	// Debug Views
	ClassDB::bind_method(D_METHOD("set_show_checkered", "enabled"), &Terrain3D::set_show_checkered);
	ClassDB::bind_method(D_METHOD("get_show_checkered"), &Terrain3D::get_show_checkered);
	ClassDB::bind_method(D_METHOD("set_show_grey", "enabled"), &Terrain3D::set_show_grey);
	ClassDB::bind_method(D_METHOD("get_show_grey"), &Terrain3D::get_show_grey);
	ClassDB::bind_method(D_METHOD("set_show_heightmap", "enabled"), &Terrain3D::set_show_heightmap);
	ClassDB::bind_method(D_METHOD("get_show_heightmap"), &Terrain3D::get_show_heightmap);
	ClassDB::bind_method(D_METHOD("set_show_colormap", "enabled"), &Terrain3D::set_show_colormap);
	ClassDB::bind_method(D_METHOD("get_show_colormap"), &Terrain3D::get_show_colormap);
	ClassDB::bind_method(D_METHOD("set_show_roughmap", "enabled"), &Terrain3D::set_show_roughmap);
	ClassDB::bind_method(D_METHOD("get_show_roughmap"), &Terrain3D::get_show_roughmap);
	ClassDB::bind_method(D_METHOD("set_show_control_texture", "enabled"), &Terrain3D::set_show_control_texture);
	ClassDB::bind_method(D_METHOD("get_show_control_texture"), &Terrain3D::get_show_control_texture);
	ClassDB::bind_method(D_METHOD("set_show_control_angle", "enabled"), &Terrain3D::set_show_control_angle);
	ClassDB::bind_method(D_METHOD("get_show_control_angle"), &Terrain3D::get_show_control_angle);
	ClassDB::bind_method(D_METHOD("set_show_control_scale", "enabled"), &Terrain3D::set_show_control_scale);
	ClassDB::bind_method(D_METHOD("get_show_control_scale"), &Terrain3D::get_show_control_scale);
	ClassDB::bind_method(D_METHOD("set_show_control_blend", "enabled"), &Terrain3D::set_show_control_blend);
	ClassDB::bind_method(D_METHOD("get_show_control_blend"), &Terrain3D::get_show_control_blend);
	ClassDB::bind_method(D_METHOD("set_show_autoshader", "enabled"), &Terrain3D::set_show_autoshader);
	ClassDB::bind_method(D_METHOD("get_show_autoshader"), &Terrain3D::get_show_autoshader);
	ClassDB::bind_method(D_METHOD("set_show_navigation", "enabled"), &Terrain3D::set_show_navigation);
	ClassDB::bind_method(D_METHOD("get_show_navigation"), &Terrain3D::get_show_navigation);
	ClassDB::bind_method(D_METHOD("set_show_texture_height", "enabled"), &Terrain3D::set_show_texture_height);
	ClassDB::bind_method(D_METHOD("get_show_texture_height"), &Terrain3D::get_show_texture_height);
	ClassDB::bind_method(D_METHOD("set_show_texture_normal", "enabled"), &Terrain3D::set_show_texture_normal);
	ClassDB::bind_method(D_METHOD("get_show_texture_normal"), &Terrain3D::get_show_texture_normal);
	ClassDB::bind_method(D_METHOD("set_show_texture_rough", "enabled"), &Terrain3D::set_show_texture_rough);
	ClassDB::bind_method(D_METHOD("get_show_texture_rough"), &Terrain3D::get_show_texture_rough);
	ClassDB::bind_method(D_METHOD("set_show_region_grid", "enabled"), &Terrain3D::set_show_region_grid);
	ClassDB::bind_method(D_METHOD("get_show_region_grid"), &Terrain3D::get_show_region_grid);
	ClassDB::bind_method(D_METHOD("set_show_instancer_grid", "enabled"), &Terrain3D::set_show_instancer_grid);
	ClassDB::bind_method(D_METHOD("get_show_instancer_grid"), &Terrain3D::get_show_instancer_grid);
	ClassDB::bind_method(D_METHOD("set_show_vertex_grid", "enabled"), &Terrain3D::set_show_vertex_grid);
	ClassDB::bind_method(D_METHOD("get_show_vertex_grid"), &Terrain3D::get_show_vertex_grid);

	// Utility
	ClassDB::bind_method(D_METHOD("get_intersection", "src_pos", "direction", "gpu_mode"), &Terrain3D::get_intersection, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("bake_mesh", "lod", "filter"), &Terrain3D::bake_mesh, DEFVAL(Terrain3DData::HEIGHT_FILTER_NEAREST));
	ClassDB::bind_method(D_METHOD("generate_nav_mesh_source_geometry", "global_aabb", "require_nav"), &Terrain3D::generate_nav_mesh_source_geometry, DEFVAL(true));

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "version", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_version");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_level", PROPERTY_HINT_ENUM, "Errors,Info,Debug,Extreme"), "set_debug_level", "get_debug_level");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "data_directory", PROPERTY_HINT_DIR), "set_data_directory", "get_data_directory");

	// Object references
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_NONE, "Terrain3DData", PROPERTY_USAGE_NONE), "", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "assets", PROPERTY_HINT_RESOURCE_TYPE, "Terrain3DAssets"), "set_assets", "get_assets");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "collision", PROPERTY_HINT_NONE, "Terrain3DCollision", PROPERTY_USAGE_NONE), "", "get_collision");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "instancer", PROPERTY_HINT_NONE, "Terrain3DInstancer", PROPERTY_USAGE_NONE), "", "get_instancer");

	ADD_GROUP("Regions", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "region_size", PROPERTY_HINT_ENUM, "64:64,128:128,256:256,512:512,1024:1024,2048:2048", PROPERTY_USAGE_EDITOR), "change_region_size", "get_region_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "save_16_bit"), "set_save_16_bit", "get_save_16_bit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "label_distance", PROPERTY_HINT_RANGE, "0.0,10000.0,0.5,or_greater"), "set_label_distance", "get_label_distance");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "label_size", PROPERTY_HINT_RANGE, "24,128,1"), "set_label_size", "get_label_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_grid"), "set_show_region_grid", "get_show_region_grid");

	ADD_GROUP("Collision", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mode", PROPERTY_HINT_ENUM, "Disabled,Dynamic / Game,Dynamic / Editor,Full / Game,Full / Editor"), "set_collision_mode", "get_collision_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_shape_size", PROPERTY_HINT_RANGE, "8,64,8"), "set_collision_shape_size", "get_collision_shape_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_radius", PROPERTY_HINT_RANGE, "16,256,16"), "set_collision_radius", "get_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_priority", PROPERTY_HINT_RANGE, "0.1,256,.1"), "set_collision_priority", "get_collision_priority");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial"), "set_physics_material", "get_physics_material");

	ADD_GROUP("Mesh", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_lods", PROPERTY_HINT_RANGE, "1,10,1"), "set_mesh_lods", "get_mesh_lods");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_size", PROPERTY_HINT_RANGE, "8,64,1"), "set_mesh_size", "get_mesh_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_RANGE, "0.25,10.0,0.05,or_greater"), "set_vertex_spacing", "get_vertex_spacing");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_render_layers", "get_render_layers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mouse_layer", PROPERTY_HINT_RANGE, "21, 32"), "set_mouse_layer", "get_mouse_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cast_shadows", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cast_shadows", "get_cast_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "gi_mode", PROPERTY_HINT_ENUM, "Disabled,Static,Dynamic"), "set_gi_mode", "get_gi_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cull_margin", PROPERTY_HINT_RANGE, "0.0,10000.0,.5,or_greater"), "set_cull_margin", "get_cull_margin");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_instances", PROPERTY_HINT_NONE), "set_show_instances", "get_show_instances");

	ADD_GROUP("Debug Views", "show_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_checkered", PROPERTY_HINT_NONE), "set_show_checkered", "get_show_checkered");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_grey", PROPERTY_HINT_NONE), "set_show_grey", "get_show_grey");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_heightmap", PROPERTY_HINT_NONE), "set_show_heightmap", "get_show_heightmap");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_colormap", PROPERTY_HINT_NONE), "set_show_colormap", "get_show_colormap");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_roughmap", PROPERTY_HINT_NONE), "set_show_roughmap", "get_show_roughmap");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_control_texture", PROPERTY_HINT_NONE), "set_show_control_texture", "get_show_control_texture");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_control_angle", PROPERTY_HINT_NONE), "set_show_control_angle", "get_show_control_angle");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_control_scale", PROPERTY_HINT_NONE), "set_show_control_scale", "get_show_control_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_control_blend", PROPERTY_HINT_NONE), "set_show_control_blend", "get_show_control_blend");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_autoshader", PROPERTY_HINT_NONE), "set_show_autoshader", "get_show_autoshader");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_navigation", PROPERTY_HINT_NONE), "set_show_navigation", "get_show_navigation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_texture_height", PROPERTY_HINT_NONE), "set_show_texture_height", "get_show_texture_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_texture_normal", PROPERTY_HINT_NONE), "set_show_texture_normal", "get_show_texture_normal");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_texture_rough", PROPERTY_HINT_NONE), "set_show_texture_rough", "get_show_texture_rough");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_region_grid", PROPERTY_HINT_NONE), "set_show_region_grid", "get_show_region_grid");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_instancer_grid", PROPERTY_HINT_NONE), "set_show_instancer_grid", "get_show_instancer_grid");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_vertex_grid", PROPERTY_HINT_NONE), "set_show_vertex_grid", "get_show_vertex_grid");

	ADD_SIGNAL(MethodInfo("material_changed"));
	ADD_SIGNAL(MethodInfo("assets_changed"));
}
