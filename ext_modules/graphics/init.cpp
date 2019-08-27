#include <cstdio>
#include <algorithm>

#include "util.hpp"

#include <Python.h>
#include <glad.h>
#include <glm/glm.hpp>
namespace sm64 {
  extern "C" {
    #include <libsm64.h>
  }
}

#include "renderer.hpp"

using sm64::s8;
using sm64::s16;
using sm64::s32;
using sm64::s64;
using sm64::u8;
using sm64::u16;
using sm64::u32;
using sm64::u64;
using sm64::f32;
using sm64::f64;


#define VEC3F_TO_VEC3(v) (vec3((v)[0], (v)[1], (v)[2]))


static PyObject *new_renderer(PyObject *self, PyObject *args) {
  static bool loaded_gl = false;

  if (!loaded_gl) {
    if (!gladLoadGL()) {
      PyErr_SetString(PyExc_Exception, "Failed to load OpenGL");
      return NULL;
    }
    loaded_gl = true;
  }

  Renderer *renderer = new Renderer;

  return PyLong_FromVoidPtr((void *)renderer);
}


static PyObject *delete_renderer(PyObject *self, PyObject *args) {
  PyObject *renderer_object;
  if (!PyArg_ParseTuple(args, "O", &renderer_object)) {
    return NULL;
  }

  Renderer *renderer = (Renderer *)PyLong_AsVoidPtr(renderer_object);
  if (PyErr_Occurred()) {
    return NULL;
  }

  delete renderer;

  Py_RETURN_NONE;
}


static void *segmented_to_virtual(sm64::SM64State *st, void *addr) {
  void *result = ((void *)0);
  s32 i = 0;
  for (; (i < 32); (i++)) {
    if (((st->sSegmentTable[i].srcStart <= addr) && (addr < st->sSegmentTable[i].srcEnd))) {
      if ((result != ((void *)0))) {
        fprintf(stderr, "Warning: segmented_to_virtual: Found two segments containing address\n");
        exit(1);
      }
      (result = ((((u8 *)addr) - ((u8 *)st->sSegmentTable[i].srcStart)) + (u8 *)st->sSegmentTable[i].dstStart));
    }
  }
  if ((result == ((void *)0))) {
    (result = addr);
  }
  return result;
}


static u32 get_object_list_from_behavior(u32 *behavior) {
  u32 objectList;

  // If the first behavior command is "begin", then get the object list header
  // from there
  if ((behavior[0] >> 24) == 0) {
    objectList = (behavior[0] >> 16) & 0xFFFF;
  } else {
    objectList = sm64::OBJ_LIST_DEFAULT;
  }

  return objectList;
}

static u32 get_object_list(sm64::Object *object) {
  return get_object_list_from_behavior((u32 *)object->behavior);
}


struct GameState {
  int frame;
  sm64::SM64State *base;
  sm64::SM64State *data;

  template<typename T>
  T *from_base(T *addr) {
    char *addr1 = (char *)addr;
    char *base1 = (char *)base;
    char *data1 = (char *)data;
    if (addr1 < base1 || addr1 >= (char *)(base + 1)) {
      return addr;
    }
    return (T *)(addr1 - base1 + data1);
  }

  bool operator==(const GameState &other) const {
    return frame == other.frame;
  }
};


struct RenderInfo {
  Camera camera;
  GameState current_state;
  vector<GameState> path_states;
};


static int read_int(int *result, PyObject *int_object) {
  if (int_object == NULL) {
    return false;
  }

  *result = (int)PyLong_AsLong(int_object);
  if (PyErr_Occurred()) {
    return false;
  }

  Py_DECREF(int_object);
  return true;
}


static bool read_float(float *result, PyObject *float_object) {
  if (float_object == NULL) {
    return false;
  }

  *result = (float)PyFloat_AsDouble(float_object);
  if (PyErr_Occurred()) {
    return false;
  }

  Py_DECREF(float_object);
  return true;
}


static bool read_vec3(vec3 *result, PyObject *vec_object) {
  if (vec_object == NULL) {
    return false;
  }

  for (int i = 0; i < 3; i++) {
    PyObject *index = PyLong_FromLong(i);
    if (index == NULL) {
      return false;
    }

    if (!read_float(&(*result)[i], PyObject_GetItem(vec_object, index))) {
      return false;
    }

    Py_DECREF(index);
  }

  Py_DECREF(vec_object);
  return true;
}

static bool read_camera(Camera *camera, PyObject *camera_object) {
  if (camera_object == NULL) {
    return false;
  }

  PyObject *mode_object = PyObject_GetAttrString(camera_object, "mode");
  if (mode_object == NULL) {
    return false;
  }
  PyObject *mode_int_object = PyObject_GetAttrString(mode_object, "value");
  if (mode_int_object == NULL) {
    return false;
  }
  camera->mode = (CameraMode)PyLong_AsLong(mode_int_object);
  if (PyErr_Occurred()) {
    return false;
  }
  Py_DECREF(mode_int_object);
  Py_DECREF(mode_object);

  switch (camera->mode) {
    case CameraMode::ROTATE: {
      if (!read_vec3(&camera->rotate_camera.pos, PyObject_GetAttrString(camera_object, "pos")) ||
        !read_float(&camera->rotate_camera.pitch, PyObject_GetAttrString(camera_object, "pitch")) ||
        !read_float(&camera->rotate_camera.yaw, PyObject_GetAttrString(camera_object, "yaw")) ||
        !read_float(&camera->rotate_camera.fov_y, PyObject_GetAttrString(camera_object, "fov_y")))
      {
        return false;
      }
      break;
    }
    case CameraMode::BIRDS_EYE: {
      if (!read_vec3(&camera->birds_eye_camera.pos, PyObject_GetAttrString(camera_object, "pos")) ||
        !read_float(&camera->birds_eye_camera.span_y, PyObject_GetAttrString(camera_object, "span_y")))
      {
        return false;
      }
      break;
    }
  }

  Py_DECREF(camera_object);
  return true;
}


static bool read_game_state(GameState *state, PyObject *state_object) {
  if (state_object == NULL) {
    return false;
  }

  if (!read_int(&state->frame, PyObject_GetAttrString(state_object, "frame"))) {
    return false;
  }

  PyObject *addr_object = PyObject_GetAttrString(state_object, "addr");
  if (addr_object == NULL) {
    return false;
  }
  state->data = (sm64::SM64State *)PyLong_AsVoidPtr(addr_object);
  if (PyErr_Occurred()) {
    return false;
  }

  PyObject *base_addr_object = PyObject_GetAttrString(state_object, "base_addr");
  if (base_addr_object == NULL) {
    return false;
  }
  state->base = (sm64::SM64State *)PyLong_AsVoidPtr(base_addr_object);
  if (PyErr_Occurred()) {
    return false;
  }

  Py_DECREF(addr_object);
  Py_DECREF(state_object);
  return true;
}


static bool read_game_state_list(vector<GameState> *states, PyObject *states_object) {
  if (states_object == NULL) {
    return false;
  }

  size_t length = PyObject_Length(states_object);
  *states = vector<GameState>(length);

  for (size_t i = 0; i < length; i++) {
    PyObject *index = PyLong_FromLong(i);
    if (index == NULL) {
      return false;
    }

    if (!read_game_state(&(*states)[i], PyObject_GetItem(states_object, index))) {
      return false;
    }

    Py_DECREF(index);
  }

  Py_DECREF(states_object);
  return true;
}


static bool read_render_args(Renderer **renderer, RenderInfo *info, PyObject *args) {
  PyObject *renderer_object, *info_object;
  if (!PyArg_ParseTuple(args, "OO", &renderer_object, &info_object)) {
    return false;
  }

  *renderer = (Renderer *)PyLong_AsVoidPtr(renderer_object);
  if (PyErr_Occurred()) {
    return false;
  }

  if (!read_camera(&info->camera, PyObject_GetAttrString(info_object, "camera"))) {
    return false;
  }

  if (!read_game_state(&info->current_state, PyObject_GetAttrString(info_object, "current_state"))) {
    return false;
  }

  if (!read_game_state_list(&info->path_states, PyObject_GetAttrString(info_object, "path_states"))) {
    return false;
  }

  return true;
}


float remove_x = 0;


static PyObject *render(PyObject *self, PyObject *args) {
  Renderer *renderer;
  RenderInfo render_info;
  RenderInfo *info = &render_info;

  if (!read_render_args(&renderer, info, args)) {
    return NULL;
  }


  Viewport viewport = {{0, 0}, {640, 480}};
  Scene scene;


  GameState st = info->current_state;

  // f32 *camera_pos = st->D_8033B328.unk0[1];
  // f32 camera_pitch = st->D_8033B328.unk4C * 3.14159f / 0x8000;
  // f32 camera_yaw = st->D_8033B328.unk4E * 3.14159f / 0x8000;
  // f32 camera_fov_y = /*D_8033B234*/ 45 * 3.14159f / 180;

  scene.camera = info->camera;


  for (s32 i = 0; i < st.data->gSurfacesAllocated; i++) {
    struct sm64::Surface *surface = &st.from_base(st.data->sSurfacePool)[i];

    SurfaceType type;
    if (surface->normal.y > 0.01) {
      type = SurfaceType::FLOOR;
    } else if (surface->normal.y < -0.01) {
      type = SurfaceType::CEILING;
    } else if (surface->normal.x < -0.707 || surface->normal.x > 0.707) {
      type = SurfaceType::WALL_X_PROJ;
    } else {
      type = SurfaceType::WALL_Z_PROJ;
    }

    scene.surfaces.push_back({
      type,
      {
        vec3(surface->vertex1[0], surface->vertex1[1], surface->vertex1[2]),
        vec3(surface->vertex2[0], surface->vertex2[1], surface->vertex2[2]),
        vec3(surface->vertex3[0], surface->vertex3[1], surface->vertex3[2]),
      },
      vec3(surface->normal.x, surface->normal.y, surface->normal.z),
    });
  }

  for (s32 i = 0; i < 240; i++) {
    sm64::Object *obj = &st.data->gObjectPool[i];
    if (obj->activeFlags & ACTIVE_FLAG_ACTIVE) {
      scene.objects.push_back({
        vec3(obj->oPosX, obj->oPosY, obj->oPosZ),
        obj->hitboxHeight,
        obj->hitboxRadius,
      });
    }
  }

  size_t current_index = std::distance(
    info->path_states.begin(),
    std::find(info->path_states.begin(), info->path_states.end(), info->current_state));

  vector<ObjectPathNode> mario_path;
  for (GameState path_st : info->path_states) {
    sm64::MarioState *m = path_st.from_base(path_st.data->gMarioState);

    if (!mario_path.empty() && mario_path.size() == current_index + 1) {
      sm64::QStepsInfo *qsteps = &path_st.data->gQStepsInfo;
      if (qsteps->numSteps > 4) {
        printf("%d\n", qsteps->numSteps);
      }
      for (int i = 0; i < qsteps->numSteps; i++) {
        mario_path.back().quarter_steps.push_back({
          VEC3F_TO_VEC3(qsteps->steps[i].intendedPos),
          VEC3F_TO_VEC3(qsteps->steps[i].resultPos),
        });
      }
    }

    mario_path.push_back({
      vec3(m->pos[0], m->pos[1], m->pos[2]),
      vector<QuarterStep>(),
    });
  }
  scene.object_paths.push_back({
    mario_path,
    current_index,
  });

  // for (s32 i = 0; i < 240; i++) {
  //   sm64::Object *obj = &st.data->gObjectPool[current_index];
  //   if (obj->activeFlags & ACTIVE_FLAG_ACTIVE) {
  //     vector<vec3> path;
  //     for (GameState path_st : info->path_states) {
  //       obj = &path_st.data->gObjectPool[i];
  //       path.push_back(vec3(obj->oPosX, obj->oPosY, obj->oPosZ));
  //     }
  //     scene.object_paths.push_back({
  //       path,
  //       current_index,
  //     });
  //   }
  // }

  renderer->render(viewport, scene);


  glUseProgram(0);

  // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glViewport(viewport.pos.x, viewport.pos.y, viewport.size.x, viewport.size.y);

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);

  f32 *camera_pos = st.data->D_8033B328.unk0[1];
  f32 camera_pitch = st.data->D_8033B328.unk4C * 3.14159f / 0x8000;
  f32 camera_yaw = st.data->D_8033B328.unk4E * 3.14159f / 0x8000;
  f32 camera_fov_y = /*D_8033B234*/ 45 * 3.14159f / 180;

  scene.camera.rotate_camera = {
    VEC3F_TO_VEC3(camera_pos),
    camera_pitch,
    camera_yaw,
    camera_fov_y,
  };

  renderer->build_transforms(viewport, scene);
  mat4 game_view_matrix = renderer->view_matrix;

  scene.camera = info->camera;
  renderer->build_transforms(viewport, scene);

  glMatrixMode(GL_PROJECTION);
  glLoadMatrixf(&renderer->proj_matrix[0][0]);

  glMatrixMode(GL_MODELVIEW);
  glLoadMatrixf(&renderer->view_matrix[0][0]);
  // glLoadIdentity();
  // glTranslatef(0, 0, -100);
  // float scale = 0.1f;
  // glScalef(scale, scale, scale);

  // glRotatef(remove_x, 0, 1, 0);
  // remove_x += 1;

  void interpret_display_list(GameState st, u32 *dl, string indent="");
  mat4 matrix_fixed_to_float(u16 *mtx);

  bool found = false;

  // printf("%p %p, %p %p\n", st.base, st.base + 1, st.data, st.data + 1);

  vec3 pos;

  for (int i = 0; i < 8; i++) {
    sm64::GraphNodeToggleZBuffer_sub *node = st.from_base(st.data->gDisplayLists.unk14[i]);
    while (node != nullptr) {
      sm64::Object *object = st.from_base(node->object);
      sm64::Object *mario_object = st.from_base(st.data->gMarioObject);

      if (mario_object != nullptr && object == mario_object) {
        u16 *transform = (u16 *)st.from_base(node->unk0);
        // printf("%p -> %p\n", node->unk4, st.from_base(node->unk4));
        u32 *display_list = (u32 *)st.from_base(node->unk4);

        // printf("%d %p %08X %08X\n", i, display_list, display_list[0], display_list[1]);

        mat4 matrix = matrix_fixed_to_float(transform);
        matrix = glm::inverse(game_view_matrix) * matrix;

        // matrix = glm::inverse(renderer->view_matrix) * matrix;
        // if (!found) {
        //   pos = vec3(matrix[3].x, matrix[3].y, matrix[3].z);
        //   // vec3 cam_pos = VEC3F_TO_VEC3(st.data->D_8033B328.unk0[1]);
        //   // printf("%f %f %f\n", pos.x, pos.y, pos.z);
        //   // printf("%f %f %f\n", cam_pos.x, cam_pos.y, cam_pos.z);
        //   // printf("\n");
        // }
        // matrix[3] = vec4(vec3(matrix[3].x, matrix[3].y, matrix[3].z) - pos, 1);
        // matrix[3] = vec4(0, 0, 0, 1);
        // for (int r = 0; r < 4; r++) {
        //   for (int c = 0; c < 4; c++) {
        //     printf("%f ", matrix[c][r]);
        //   }
        //   printf("\n");
        // }
        // printf("\n");
        // for (int r = 0; r < 4; r++) {
        //   for (int c = 0; c < 4; c++) {
        //     printf("%f ", renderer->view_matrix[c][r]);
        //   }
        //   printf("\n");
        // }
        // printf("pos = %f %f %f \n", mario_path[current_index].pos.x, mario_path[current_index].pos.y, mario_path[current_index].pos.z);
        // vec3 cam_pos = VEC3F_TO_VEC3(st.data->D_8033B328.unk0[1]);
        // printf("cam pos = %f %f %f\n", cam_pos.x, cam_pos.y, cam_pos.z);
        // // printf("\n");
        // printf("\n\n");

        glPushMatrix();
        glMultMatrixf(&matrix[0][0]);

        interpret_display_list(st, display_list);

        glPopMatrix();

        found = true;
      }

      node = st.from_base(node->unk8);
    }
  }

  // printf("\n");

  // if (found) {
  //   exit(0);
  // }

  // static bool done = false;
  // if (!done) {
  //   if (st.data->gMarioObject != NULL) {
  //     sm64::Object *object = st.from_base(st.data->gMarioObject);
  //     if (object != nullptr) {
  //       printf("%p %p\n", object->displayListStart, object->displayListEnd);
  //       // done = true;
  //       // u32 *dl = (u32 *)object->displayListStart;
  //       // while (dl < (u32 *)object->displayListEnd) {
  //       //   printf("0x%08X\n", dl);
  //       // }
  //     }
  //   }
  // }

  Py_RETURN_NONE;
}


vector<vec3> loaded_vertices(32);


mat4 matrix_fixed_to_float(u16 *mtx) {
  mat4 result;
  for (size_t i = 0; i < 16; i++) {
    s32 val32 = (s32)((mtx[i] << 16) + mtx[16 + i]);
    result[i / 4][i % 4] = (f32)val32 / 0x10000;
  }
  return result;
}


void interpret_display_list(GameState st, u32 *dl, string indent) {
  // printf("%s-----\n", indent.c_str());

  while (true) {
    u32 w0 = dl[0];
    u32 w1 = dl[1];
    u8 cmd = w0 >> 24;

    // printf("%s%08X %08X\n", indent.c_str(), w0, w1);

    switch (cmd) {
    case 0x01: { // gSPMatrix
      fprintf(stderr, "gSPMatrix\n");
      exit(1);

      // u8 p = (w0 >> 16) & 0xFF;
      // u16 *fixed_point = st.from_base((u16 *)w1);
      // mat4 matrix = matrix_fixed_to_float(fixed_point);

      // glMatrixMode((p & 0x01) ? GL_PROJECTION : GL_MODELVIEW);

      // if (p & 0x04) {
      //   glPushMatrix();
      // } else {
      //   // no push
      // }

      // if (p & 0x02) {
      //   // load
      //   fprintf(stderr, "gSPMatrix load\n");
      //   exit(1);
      // } else {
      //   glMultMatrixf(&matrix[0][0]);
      // }

      break;
    }

    case 0x03: // gSPViewport, gSPLight
      break;

    case 0x04: { // gSPVertex
      u32 n = ((w0 >> 20) & 0xF) + 1;
      u32 v0 = (w0 >> 16) & 0xF;
      sm64::Vtx *v = st.from_base((sm64::Vtx *)w1);

      loaded_vertices.clear();
      for (u32 i = 0; i < n; i++) {
        loaded_vertices[v0 + i] = vec3(v[i].v.ob[0], v[i].v.ob[1], v[i].v.ob[2]);
      }

      break;
    }

    case 0x06: { // gSPDisplayList, gSPBranchList
      u32 *new_dl = st.from_base((u32 *)w1);
      if (w0 == 0x06000000) {
        interpret_display_list(st, new_dl, indent + "  ");
      } else if (w0 == 0x06010000) {
        dl = new_dl - 2;
      } else {
        fprintf(stderr, "gSPDisplayList: 0x%08X\n", w0);
        exit(1);
      }
      break;
    }

    case 0xB6: // gSPClearGeometryMode
      break;

    case 0xB7: // gSPSetGeometryMode
      break;

    case 0xB8: // gSPEndDisplayList
      return;

    case 0xB9: // gDPSetAlphaCompare, gDPSetDepthSource, gDPSetRenderMode
      break;

    case 0xBB: // gSPTexture
      break;

    case 0xBF: { // gSP1Triangle
      u32 v0 = ((w1 >> 16) & 0xFF) / 10;
      u32 v1 = ((w1 >> 8) & 0xFF) / 10;
      u32 v2 = ((w1 >> 0) & 0xFF) / 10;

      glBegin(GL_LINE_LOOP);
      glVertex3f(loaded_vertices[v0].x, loaded_vertices[v0].y, loaded_vertices[v0].z);
      glVertex3f(loaded_vertices[v1].x, loaded_vertices[v1].y, loaded_vertices[v1].z);
      glVertex3f(loaded_vertices[v2].x, loaded_vertices[v2].y, loaded_vertices[v2].z);
      glEnd();

      break;
    }

    case 0xE6: // gDPLoadSync
      break;

    case 0xE7: // gDPPipeSync
      break;

    case 0xE8: // gDPTileSync
      break;

    case 0xF2: // gDPSetTileSize
      break;

    case 0xF3: // gDPLoadBlock
      break;

    case 0xF5: // gDPSetTile
      break;

    case 0xFB: // gDPSetEnvColor
      break;

    case 0xFC: // gDPSetCombineMode
      break;

    case 0xFD: // gDPSetTextureImage
      break;

    default:
      // fprintf(stderr, "0x%02X\n", cmd);
      // exit(1);
      break;
    }

    dl += 2;
  }
}


static PyMethodDef method_defs[] = {
  { "new_renderer", new_renderer, METH_NOARGS, NULL },
  { "delete_renderer", delete_renderer, METH_VARARGS, NULL },
  { "render", render, METH_VARARGS, NULL },
  { NULL, NULL, 0, NULL },
};


static PyModuleDef module_def = {
  PyModuleDef_HEAD_INIT,
  "graphics",
  NULL,
  -1,
  method_defs,
};


PyMODINIT_FUNC PyInit_graphics(void) {
  return PyModule_Create(&module_def);
}
