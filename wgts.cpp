#define GL_GLEXT_PROTOTYPES

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <csignal>
#include <pthread.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#include <GL/gl.h>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "xdg-shell.h"

static const char *default_vert = R"(
#version 120
void main() {
    gl_Position = gl_Vertex;
}
)";

static const char *default_frag = R"(
#version 120
uniform vec2  u_resolution;
uniform float u_time;

void main() {
    vec2 fragCoord = gl_FragCoord.xy;
    vec2 center = u_resolution * 0.5;
    float dist = distance(fragCoord, center);
    float radius = 50.0;
    if (dist < radius) {
        vec3 color = vec3(1.0, 0.4, 0.7);
        gl_FragColor = vec4(color, 0.9);
    } else {
        gl_FragColor = vec4(0.0);
    }
}
)";
#pragma region Settings
struct Settings {
  std::unordered_map<std::string, std::string> kv;

  bool load(const std::string &path) {
    std::ifstream f(path);
    if (!f)
      return false;

    std::string line;
    while (std::getline(f, line)) {
      if (line.empty())
        continue;

      auto eq = line.find('=');
      if (eq == std::string::npos)
        continue;

      std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);

      trim(key);
      trim(val);

      kv[key] = val;
    }
    return true;
  }

  bool save(const std::string &path) const {
    std::ofstream f(path);
    if (!f)
      return false;

    for (auto &[k, v] : kv)
      f << k << " = " << v << "\n";

    return true;
  }

  int get_int(const std::string &key, int def) const {
    auto it = kv.find(key);
    if (it == kv.end())
      return def;
    return std::stoi(it->second);
  }

  std::string get_string(const std::string &key, const std::string &def) const {
    auto it = kv.find(key);
    if (it == kv.end())
      return def;
    return it->second;
  }

  void set_int(const std::string &key, int v) { kv[key] = std::to_string(v); }

  void set_string(const std::string &key, const std::string &v) { kv[key] = v; }

private:
  static void trim(std::string &s) {
    const char *ws = " \t\n\r";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
  }
};

std::string get_config_file(const std::string &relative_file) {
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  std::string base;

  if (xdg && xdg[0] != '\0') {
    base = xdg;
  } else {
    const char *home = std::getenv("HOME");
    if (!home || home[0] == '\0') {
      return "";
    }
    base = std::string(home) + "/.config";
  }

  return base + "/wayshaders/" + relative_file;
}

#pragma endregion

static bool debug = false;
void logDebug(const char *format, ...) {
  if (!debug)
    return;

  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  printf("\n");
  fflush(stdout);
}

static constexpr int palette_count = 18;

static const char *palette_uniform_name(int i, char (&buf)[12]) {
  if (i == 16)
    return "u_background";
  if (i == 17)
    return "u_foreground";
  snprintf(buf, sizeof(buf), "u_color%d", i);
  return buf;
}

struct ShaderLayer {
  // draw()-hot fields first so each layer stays within one cache line
  GLuint prog;
  GLint u_time;
  GLint u_frame;
  GLuint fbo[2];
  GLuint texture[2];
  int current_buffer;
  int num; // layer num, 0 = base
  bool multipass;
  bool enabled;
  std::vector<int> enabled_channels;

  GLint u_resolution;
  int setting_wrap_t;
  int setting_wrap_s;
  GLint u_palette[palette_count];
  bool uses_palette;
};
struct client_state {
  Settings settings;
  wl_display *display;
  wl_registry *registry;
  wl_compositor *compositor;

  xdg_wm_base *wm_base;
  wl_surface *surface;
  xdg_surface *xdg_surface_obj;
  xdg_toplevel *toplevel;

  wl_egl_window *egl_window;
  EGLDisplay egl_display;
  EGLConfig egl_config;
  EGLContext egl_context;
  EGLSurface egl_surface;

  int width;
  int height;
  bool running;
  int frame_count = 0;

  std::vector<ShaderLayer> layers;
  GLuint quad_vbo;

  std::string palette_path;
  float palette[palette_count][3];
  EGLContext palette_reload_context;
};

static bool parse_hex_color(const char *s, float out[3]) {
  while (*s == ' ' || *s == '\t')
    s++;
  if (*s++ != '#')
    return false;
  int nibbles[6];
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    if (c >= '0' && c <= '9')
      nibbles[i] = c - '0';
    else if (c >= 'a' && c <= 'f')
      nibbles[i] = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      nibbles[i] = c - 'A' + 10;
    else
      return false;
  }
  out[0] = (nibbles[0] << 4 | nibbles[1]) * (1.f / 255.f);
  out[1] = (nibbles[2] << 4 | nibbles[3]) * (1.f / 255.f);
  out[2] = (nibbles[4] << 4 | nibbles[5]) * (1.f / 255.f);
  return true;
}

static bool load_palette(client_state *st) {
  FILE *f = fopen(st->palette_path.c_str(), "rb");
  if (!f)
    return false;
  char buf[4096];
  size_t len = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[len] = '\0';

  float staged[palette_count][3];
  int loaded = 0;
  char *line = buf;
  while (loaded < palette_count && line) {
    char *newline = strchr(line, '\n');
    if (newline)
      *newline = '\0';
    if (parse_hex_color(line, staged[loaded]))
      loaded++;
    line = newline ? newline + 1 : nullptr;
  }
  if (loaded != palette_count)
    return false;
  memcpy(st->palette, staged, sizeof(staged));
  return true;
}

static void apply_palette(client_state *st) {
  for (ShaderLayer &shader : st->layers) {
    if (!shader.uses_palette)
      continue;
    for (int i = 0; i < palette_count; i++) {
      if (shader.u_palette[i] != -1)
        glProgramUniform3fv(shader.prog, shader.u_palette[i], 1,
                            st->palette[i]);
    }
  }
}

static void *palette_reload_thread(void *arg) {
  client_state *st = (client_state *)arg;
  eglBindAPI(EGL_OPENGL_API);
  if (!eglMakeCurrent(st->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      st->palette_reload_context)) {
    logDebug("Palette reload unavailable: surfaceless make-current failed");
    return nullptr;
  }
  sigset_t hup;
  sigemptyset(&hup);
  sigaddset(&hup, SIGHUP);
  int sig;
  while (sigwait(&hup, &sig) == 0) {
    if (!load_palette(st))
      continue;
    apply_palette(st);
    glFlush();
    logDebug("Palette reloaded from %s", st->palette_path.c_str());
  }
  return nullptr;
}

static void start_palette_reload_thread(client_state *st) {
  sigset_t hup;
  sigemptyset(&hup);
  sigaddset(&hup, SIGHUP);
  pthread_sigmask(SIG_BLOCK, &hup, nullptr);
  st->palette_reload_context = eglCreateContext(
      st->egl_display, st->egl_config, st->egl_context, nullptr);
  if (st->palette_reload_context == EGL_NO_CONTEXT) {
    logDebug("Palette reload unavailable: shared EGL context failed");
    return;
  }
  pthread_t thread;
  pthread_create(&thread, nullptr, palette_reload_thread, st);
  pthread_detach(thread);
}

static void init_multipass(client_state *st) {
  for (ShaderLayer &shader : st->layers) {
    logDebug("Process shader%d multipass", shader.num);
    glUseProgram(shader.prog);
    glUniform2f(shader.u_resolution, st->width, st->height);
    if (shader.multipass) {
      glGenFramebuffers(2, shader.fbo);
      glGenTextures(2, shader.texture);

      for (int i = 0; i < 2; i++) {
        // Texture
        glBindTexture(GL_TEXTURE_2D, shader.texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, st->width, st->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        shader.setting_wrap_s);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        shader.setting_wrap_t);

        // Texture to framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, shader.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, shader.texture[i], 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
          logDebug("Framebuffer %d not complete!", i);
        }
      }

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      shader.current_buffer = 0;

      // Check all possible channel vars
      for (std::size_t i = 0; i < st->layers.size(); i++) {
        logDebug("%d loop", i);
        std::string name = "u_sampler" + std::to_string(i);
        GLint channel = glGetUniformLocation(shader.prog, name.c_str());
        if (channel != -1) {
          glUniform1i(channel, i);
          logDebug("Found %s in Shader %d", name.c_str(), shader.num);
          // track enabled channel
          shader.enabled_channels.push_back(i);
        }
      }
    }
  }
}

static void resize_multipass(client_state *st) {
  logDebug("Resized window!");
  for (ShaderLayer &shader : st->layers) {
    glUseProgram(shader.prog);
    glUniform2f(shader.u_resolution, st->width, st->height);
    if (shader.multipass) {
      glDeleteTextures(2, shader.texture);
      // My textures yay
      glGenTextures(2, shader.texture);

      for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, shader.texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, st->width, st->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        shader.setting_wrap_s);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        shader.setting_wrap_t);

        // Reattach to framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, shader.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, shader.texture[i], 0);
      }

      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      logDebug("Resized FBO textures to %dx%d", st->width, st->height);
    }
  }
}

std::string load_shader_from_file(const std::string &filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

double get_time_seconds() {
  using clock = std::chrono::steady_clock;
  static const auto start_time = clock::now();
  return std::chrono::duration<double>(clock::now() - start_time).count();
}

static void registry_global(void *data, wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
  client_state *st = (client_state *)data;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    st->compositor = (wl_compositor *)wl_registry_bind(
        registry, name, &wl_compositor_interface, 4);
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    st->wm_base = (xdg_wm_base *)wl_registry_bind(registry, name,
                                                  &xdg_wm_base_interface, 1);
  }
}

static void registry_remove(void *, wl_registry *, uint32_t) {}

static const wl_registry_listener registry_listener = {registry_global,
                                                       registry_remove};

// Shell listeners
static void wm_base_ping(void *, xdg_wm_base *base, uint32_t serial) {
  xdg_wm_base_pong(base, serial);
}

static const xdg_wm_base_listener wm_base_listener = {wm_base_ping};

static void xdg_surface_configure(void *, xdg_surface *surface,
                                  uint32_t serial) {
  xdg_surface_ack_configure(surface, serial);
}

static const xdg_surface_listener xdg_surface_listener = {
    xdg_surface_configure};

static void toplevel_configure(void *data, xdg_toplevel *, int32_t width,
                               int32_t height, wl_array *) {
  client_state *st = (client_state *)data;

  if (width > 0 && height > 0 &&
      (width != st->width || height != st->height)) {
    st->width = width;
    st->height = height;
    // logDebug("Window resize");
    resize_multipass(st);

    wl_egl_window_resize(st->egl_window, width, height, 0, 0);
    glViewport(0, 0, width, height);
  }
}

static void toplevel_close(void *data, xdg_toplevel *) {
  ((client_state *)data)->running = false;
}

static const xdg_toplevel_listener toplevel_listener = {toplevel_configure,
                                                        toplevel_close};

// EGL
static void init_egl(client_state *st) {
  st->egl_display = eglGetDisplay((EGLNativeDisplayType)st->display);
  eglInitialize(st->egl_display, nullptr, nullptr);

  const EGLint cfg[] = {EGL_SURFACE_TYPE,
                        EGL_WINDOW_BIT,
                        EGL_RENDERABLE_TYPE,
                        EGL_OPENGL_BIT,
                        EGL_RED_SIZE,
                        8,
                        EGL_GREEN_SIZE,
                        8,
                        EGL_BLUE_SIZE,
                        8,
                        EGL_ALPHA_SIZE,
                        8,
                        EGL_NONE};

  EGLint count;
  eglChooseConfig(st->egl_display, cfg, &st->egl_config, 1, &count);

  eglBindAPI(EGL_OPENGL_API);
  st->egl_context = eglCreateContext(st->egl_display, st->egl_config,
                                     EGL_NO_CONTEXT, nullptr);

  st->egl_window = wl_egl_window_create(st->surface, st->width, st->height);

  st->egl_surface =
      eglCreateWindowSurface(st->egl_display, st->egl_config,
                             (EGLNativeWindowType)st->egl_window, nullptr);

  eglMakeCurrent(st->egl_display, st->egl_surface, st->egl_surface,
                 st->egl_context);

  eglSwapInterval(st->egl_display, 0);
}

static GLuint compile_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);

  GLint success;
  glGetShaderiv(s, GL_COMPILE_STATUS, &success);
  if (!success) {
    char log[512];
    glGetShaderInfoLog(s, 512, NULL, log);
    logDebug("Shader compilation failed:\n%s", log);
  } else {
    logDebug("Shader compiled");
  }
  return s;
}

static GLuint create_program(const char *frag_shader, const char *vert_shader) {
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_shader);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_shader);

  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);

  glDeleteShader(vs);
  glDeleteShader(fs);
  return prog;
}

static void init_quad(client_state *st) {
  const float quad_verts[] = {
      -1.f, -1.f, 0.f, 0.f, 1.f, -1.f, 1.f, 0.f,
      -1.f, 1.f,  0.f, 1.f, 1.f, 1.f,  1.f, 1.f,
  };
  glGenBuffers(1, &st->quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, st->quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts,
               GL_STATIC_DRAW);
  glVertexPointer(2, GL_FLOAT, 4 * sizeof(float), (void *)0);
  glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float),
                    (void *)(2 * sizeof(float)));
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  glClearColor(0.f, 0.f, 0.f, 0.f);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_TEXTURE_2D);
  glViewport(0, 0, st->width, st->height);
}

static void draw(client_state *st) {
  static const double start = get_time_seconds();
  const float t = (float)(get_time_seconds() - start);
  const float frame = (float)st->frame_count;

  // Multipass
  for (ShaderLayer &shader : st->layers) {
    if (!shader.enabled || !shader.multipass)
      continue;

    const int read_buffer = shader.current_buffer;
    const int write_buffer = 1 - read_buffer;

    glUseProgram(shader.prog);

    glBindFramebuffer(GL_FRAMEBUFFER, shader.fbo[write_buffer]);

    for (const int i : shader.enabled_channels) {
      glActiveTexture(GL_TEXTURE0 + i);
      const int buffer =
          (i == shader.num) ? read_buffer : st->layers[i].current_buffer;
      glBindTexture(GL_TEXTURE_2D, st->layers[i].texture[buffer]);
    }

    if (shader.u_time != -1)
      glUniform1f(shader.u_time, t);
    if (shader.u_frame != -1)
      glUniform1f(shader.u_frame, frame);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    shader.current_buffer = write_buffer;
  }

  // Composite
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  glEnable(GL_BLEND);
  glActiveTexture(GL_TEXTURE0);

  for (const ShaderLayer &shader : st->layers) {
    if (!shader.enabled)
      continue;

    if (shader.multipass) {
      glUseProgram(0);
      glBindTexture(GL_TEXTURE_2D, shader.texture[shader.current_buffer]);

      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
      glUseProgram(shader.prog);
      if (shader.u_time != -1)
        glUniform1f(shader.u_time, t);
      if (shader.u_frame != -1)
        glUniform1f(shader.u_frame, frame);

      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }

  glDisable(GL_BLEND);

  if (debug) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
      logDebug("OpenGL error: 0x%x", err);
    }
  }

  eglSwapBuffers(st->egl_display, st->egl_surface);
  st->frame_count++;
}

static void frame_done(void *data, wl_callback *cb, uint32_t);

static const wl_callback_listener frame_listener = {frame_done};

static void request_frame(client_state *st) {
  wl_callback *cb = wl_surface_frame(st->surface);
  wl_callback_add_listener(cb, &frame_listener, st);
}

static void frame_done(void *data, wl_callback *cb, uint32_t) {
  client_state *st = (client_state *)data;
  wl_callback_destroy(cb);
  request_frame(st);
  draw(st);
}

int main(int argc, char *argv[]) {
  if (argc > 1) {
    std::string_view arg = argv[1];
    if (arg == "-h" || arg == "--help") {
      printf("Displays shaders in a transparent window for Wayland. Place your "
             "shaders (up to 32) numbered as 'shader0.frag', 'shader1.frag', "
             "etc. in XDG_CONFIG_HOME/wayshaders or usually "
             "$HOME/.config/wayshaders.\n"
             "You may provide shader[n].vert or a default will be used. "
             "Settings generated as 'wayshaders' in same directory.\n"
             "\nCreated by Genevra Rose\n");
      return 0;
    }
  }
  std::string programClass = "";

  client_state st{};
  st.settings = Settings();
  if (!st.settings.load(get_config_file("wayshaders"))) {
    st.settings.set_int("debug", 0);
    st.settings.set_string("class", "wgts");
    st.settings.set_int("wrap_t0", GL_CLAMP_TO_EDGE); // GL_CLAMP_TO_EDGE);
    st.settings.set_int("wrap_s0", GL_CLAMP_TO_EDGE);
    st.settings.save(get_config_file("wayshaders"));
  } else {
    debug = st.settings.get_int("debug", 0);
    programClass = st.settings.get_string("class", "wgts");
  }
  st.width = 700;
  st.height = 400;
  st.running = true;
  st.palette_path = get_config_file("colors");
  for (int i = 0; i < palette_count; i++) {
    float def = (i == 16) ? 0.f : 0.85f;
    st.palette[i][0] = st.palette[i][1] = st.palette[i][2] = def;
  }

  st.display = wl_display_connect(nullptr);
  st.registry = wl_display_get_registry(st.display);
  wl_registry_add_listener(st.registry, &registry_listener, &st);
  wl_display_roundtrip(st.display);

  xdg_wm_base_add_listener(st.wm_base, &wm_base_listener, nullptr);

  st.surface = wl_compositor_create_surface(st.compositor);

  st.xdg_surface_obj = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);

  xdg_surface_add_listener(st.xdg_surface_obj, &xdg_surface_listener, nullptr);

  st.toplevel = xdg_surface_get_toplevel(st.xdg_surface_obj);

  xdg_toplevel_set_title(st.toplevel, "wgts");
  xdg_toplevel_set_app_id(st.toplevel, programClass.c_str());
  xdg_toplevel_add_listener(st.toplevel, &toplevel_listener, &st);

  wl_surface_commit(st.surface);

  init_egl(&st);
  init_quad(&st);

  int shader_num = 0;
  while (true) {
    std::string shader_filename =
        "shader" + std::to_string(shader_num) + ".frag";
    std::string shader_code =
        load_shader_from_file(get_config_file(shader_filename));

    if (shader_code.empty()) {
      // Will be num of loaded because ++ last run
      logDebug("Shader loading complete - found %d shader(s)", shader_num);
      break;
    }

    logDebug("Found %s", shader_filename.c_str());

    // Check for corresponding vertex shader
    std::string vert_filename = "shader" + std::to_string(shader_num) + ".vert";
    std::string vert_code =
        load_shader_from_file(get_config_file(vert_filename));

    if (vert_code.empty()) {
      logDebug("No %s found - using default vertex shader",
               vert_filename.c_str());
      vert_code = default_vert;
    } else {
      logDebug("Found %s", vert_filename.c_str());
    }

    ShaderLayer layer {};
    layer.enabled = true;
    layer.num = shader_num;
    layer.prog = create_program(shader_code.c_str(), vert_code.c_str());
    layer.u_resolution = glGetUniformLocation(layer.prog, "u_resolution");
    layer.u_time = glGetUniformLocation(layer.prog, "u_time");
    layer.u_frame = glGetUniformLocation(layer.prog, "u_frame");
    layer.uses_palette = false;
    char uniform_name[12];
    for (int i = 0; i < palette_count; i++) {
      layer.u_palette[i] = glGetUniformLocation(
          layer.prog, palette_uniform_name(i, uniform_name));
      layer.uses_palette |= layer.u_palette[i] != -1;
    }
    layer.fbo[0] = 0;
    layer.fbo[1] = 0;
    layer.texture[0] = 0;
    layer.texture[1] = 0;
    layer.current_buffer = 0;

    // Multipass enabling
    for (int i = 0; i <= shader_num; ++i) {
      std::string channel_name = "u_sampler" + std::to_string(i);
      GLint channel_uniform =
          glGetUniformLocation(layer.prog, channel_name.c_str());

      if (channel_uniform != -1) {
        logDebug("Layer %d - Multipass ON (found %s)", i, channel_name.c_str());
        if (i == shader_num)
          layer.multipass = true;
        else
          st.layers[i].multipass = true;
      } else {
        logDebug("Layer %d - Multipass OFF. No detected %s in shader.", i,
                 channel_name.c_str());
      }
    }

    GLint success;
    glGetProgramiv(layer.prog, GL_LINK_STATUS, &success);
    if (!success) {
      char log[512];
      glGetProgramInfoLog(layer.prog, 512, NULL, log);
      logDebug("Shader linking for %d failed:\n%s", shader_num, log);
      exit(-1);
    } else {
      logDebug("Shader link for %d success", shader_num);
    }

    layer.setting_wrap_t = st.settings.get_int(
        "wrap_t" + std::to_string(shader_num), GL_REPEAT); // GL_CLAMP_TO_EDGE);
    layer.setting_wrap_s = st.settings.get_int(
        "wrap_s" + std::to_string(shader_num), GL_REPEAT); // GL_CLAMP_TO_EDGE);
    st.layers.push_back(layer);
    shader_num++;
  }

  if (st.layers.empty()) {
    printf("ERROR: No shaders found! Need at least shader0.frag\n");
    return 1;
  }

  init_multipass(&st);
  load_palette(&st);
  apply_palette(&st);
  start_palette_reload_thread(&st);

  request_frame(&st);
  draw(&st);

  while (st.running && wl_display_dispatch(st.display) != -1) {
  }

  eglDestroySurface(st.egl_display, st.egl_surface);
  eglDestroyContext(st.egl_display, st.egl_context);
  wl_egl_window_destroy(st.egl_window);

  xdg_toplevel_destroy(st.toplevel);
  xdg_surface_destroy(st.xdg_surface_obj);
  wl_surface_destroy(st.surface);
  wl_display_disconnect(st.display);

  return 0;
}
