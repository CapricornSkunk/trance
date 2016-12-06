﻿#include "director.h"
#include <algorithm>
#include <iostream>
#include "font.h"
#include "session.h"
#include "theme.h"
#include "util.h"
#include "visual.h"
#include "visual_api.h"
#include "visual_cyclers.h"

#pragma warning(push, 0)
extern "C" {
#include <GL/glew.h>
}
#include <src/trance.pb.h>
#include <SFML/OpenGL.hpp>
#pragma warning(pop)

static const uint32_t spiral_type_max = 7;
#include "shaders.h"

Director::Director(sf::RenderWindow& window, const trance_pb::Session& session,
                   const trance_pb::System& system, ThemeBank& themes,
                   const trance_pb::Program& program, bool realtime, bool oculus_rift,
                   bool convert_to_yuv)
: _window{window}
, _session{session}
, _system{system}
, _themes{themes}
, _width{window.getSize().x}
, _height{window.getSize().y}
, _program{&program}
, _realtime{realtime}
, _convert_to_yuv{convert_to_yuv}
, _render_fbo{0}
, _render_fb_tex{0}
, _yuv_fbo{0}
, _yuv_fb_tex{0}
, _image_program{0}
, _spiral_program{0}
, _quad_buffer{0}
, _tex_buffer{0}
{
  _oculus.enabled = false;
  _oculus.session = nullptr;

  GLenum ok = glewInit();
  if (ok != GLEW_OK) {
    std::cerr << "couldn't initialise GLEW: " << glewGetErrorString(ok) << std::endl;
  }

  if (!GLEW_VERSION_2_1) {
    std::cerr << "OpenGL 2.1 not available" << std::endl;
  }

  if (!GLEW_ARB_texture_non_power_of_two) {
    std::cerr << "OpenGL non-power-of-two textures not available" << std::endl;
  }

  if (!GLEW_ARB_shading_language_100 || !GLEW_ARB_shader_objects || !GLEW_ARB_vertex_shader ||
      !GLEW_ARB_fragment_shader) {
    std::cerr << "OpenGL shaders not available" << std::endl;
  }

  if (!GLEW_EXT_framebuffer_object) {
    std::cerr << "OpenGL framebuffer objects not available" << std::endl;
  }

  if (oculus_rift) {
    if (_realtime) {
      _oculus.enabled = init_oculus_rift();
    } else {
      _oculus.enabled = true;
    }
  }
  if (!_realtime) {
    init_framebuffer(_render_fbo, _render_fb_tex, _width, _height);
    init_framebuffer(_yuv_fbo, _yuv_fb_tex, _width, _height);
    _screen_data.reset(new uint8_t[4 * _width * _height]);
  }

  std::cout << "\npreloading GPU" << std::endl;
  static const std::size_t gl_preload = 1000;
  for (std::size_t i = 0; i < gl_preload; ++i) {
    themes.get_image(false);
    themes.get_image(true);
  }

  auto compile_shader = [&](GLuint shader) {
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
      GLint log_size = 0;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_size);

      char* error_log = new char[log_size];
      glGetShaderInfoLog(shader, log_size, &log_size, error_log);
      std::cerr << error_log;
      delete[] error_log;
    }
  };

  auto link = [&](GLuint program) {
    glLinkProgram(program);
    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
      GLint log_size = 0;
      glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_size);

      char* error_log = new char[log_size];
      glGetProgramInfoLog(program, log_size, &log_size, error_log);
      std::cerr << error_log;
      delete[] error_log;
    }
  };

  auto compile = [&](const std::string& vertex_text, const std::string& fragment_text) {
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);

    const char* v = vertex_text.data();
    const char* f = fragment_text.data();

    glShaderSource(vertex, 1, &v, nullptr);
    glShaderSource(fragment, 1, &f, nullptr);

    compile_shader(vertex);
    compile_shader(fragment);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    return program;
  };

  _new_program = compile(new_vertex, new_fragment);
  _spiral_program = compile(spiral_vertex, spiral_fragment);
  _image_program = compile(image_vertex, image_fragment);
  _text_program = compile(text_vertex, text_fragment);
  _yuv_program = compile(yuv_vertex, yuv_fragment);

  static const float quad_data[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f,
                                    1.f,  -1.f, 1.f, 1.f,  -1.f, 1.f};
  glGenBuffers(1, &_quad_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, _quad_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 12, quad_data, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  static const float tex_data[] = {0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f};
  glGenBuffers(1, &_tex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, _tex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 12, tex_data, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  _visual_api.reset(new VisualApiImpl{*this, _themes, session, system});
  change_visual(0);
  if (_realtime && !_oculus.enabled) {
    _window.setVisible(true);
    _window.setActive();
    _window.display();
  }
}

Director::~Director()
{
  if (_oculus.session) {
    ovr_Destroy(_oculus.session);
  }
}

void Director::set_program(const trance_pb::Program& program)
{
  _program = &program;
}

bool Director::update()
{
  _visual_api->update();
  if (_old_visual) {
    _old_visual.reset(nullptr);
  }

  _visual->cycler()->advance();
  if (_visual->cycler()->complete()) {
    change_visual(_visual->cycler()->length());
  }

  bool to_oculus = _realtime && _oculus.enabled;
  if (to_oculus) {
    ovrSessionStatus status;
    auto result = ovr_GetSessionStatus(_oculus.session, &status);
    if (result != ovrSuccess) {
      std::cerr << "Oculus session status failed" << std::endl;
    }
    if (status.ShouldQuit) {
      return false;
    }
    if (status.DisplayLost) {
      std::cerr << "Oculus display lost" << std::endl;
    }
    if (status.ShouldRecenter) {
      if (ovr_RecenterTrackingOrigin(_oculus.session) != ovrSuccess) {
        ovr_ClearShouldRecenterFlag(_oculus.session);
      }
    }
    _oculus.started = status.HmdPresent && !status.DisplayLost;
    if (!status.IsVisible && random_chance(1024)) {
      std::cerr << "Lost focus (move the HMD?)" << std::endl;
    }
  }
  return true;
}

void Director::render() const
{
  Image::delete_textures();
  bool to_window = _realtime && !_oculus.enabled;
  bool to_oculus = _realtime && _oculus.enabled;

  if (!_oculus.enabled) {
    glBindFramebuffer(GL_FRAMEBUFFER, to_window ? 0 : _render_fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    _oculus.rendering_right = false;
    _visual->render(*_visual_api);
  } else if (to_oculus) {
    if (_oculus.started) {
      auto timing = ovr_GetPredictedDisplayTime(_oculus.session, 0);
      auto sensorTime = ovr_GetTimeInSeconds();
      auto tracking = ovr_GetTrackingState(_oculus.session, timing, true);
      ovr_CalcEyePoses(tracking.HeadPose.ThePose, _oculus.eye_view_offset,
                       _oculus.layer.RenderPose);

      int index = 0;
      auto result =
          ovr_GetTextureSwapChainCurrentIndex(_oculus.session, _oculus.texture_chain, &index);
      if (result != ovrSuccess) {
        std::cerr << "Oculus texture swap chain index failed" << std::endl;
      }

      glBindFramebuffer(GL_FRAMEBUFFER, _oculus.fbo_ovr[index]);
      glClear(GL_COLOR_BUFFER_BIT);

      for (int eye = 0; eye < 2; ++eye) {
        _oculus.rendering_right = eye == ovrEye_Right;
        const auto& view = _oculus.layer.Viewport[eye];
        glViewport(view.Pos.x, view.Pos.y, view.Size.w, view.Size.h);
        _visual->render(*_visual_api);
      }

      result = ovr_CommitTextureSwapChain(_oculus.session, _oculus.texture_chain);
      if (result != ovrSuccess) {
        std::cerr << "Oculus commit texture swap chain failed" << std::endl;
      }

      _oculus.layer.SensorSampleTime = sensorTime;
      const ovrLayerHeader* layers = &_oculus.layer.Header;
      result = ovr_SubmitFrame(_oculus.session, 0, nullptr, &layers, 1);
      if (result != ovrSuccess && result != ovrSuccess_NotVisible) {
        std::cerr << "Oculus submit frame failed" << std::endl;
      }
    }
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, to_window ? 0 : _render_fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int eye = 0; eye < 2; ++eye) {
      _oculus.rendering_right = eye == ovrEye_Right;
      glViewport(_oculus.rendering_right * view_width(), 0, view_width(), _height);
      _visual->render(*_visual_api);
    }
  }

  if (!_realtime) {
    // Could do more on the GPU e.g. scaling, splitting planes, but the VP8
    // encoding is the bottleneck anyway.
    glBindFramebuffer(GL_FRAMEBUFFER, _yuv_fbo);
    glViewport(0, 0, _width, _height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glUseProgram(_yuv_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _render_fb_tex);

    glUniform1f(glGetUniformLocation(_yuv_program, "yuv_mix"), _convert_to_yuv ? 1.f : 0.f);
    glUniform2f(glGetUniformLocation(_yuv_program, "resolution"), float(_width), float(_height));
    auto loc = glGetAttribLocation(_yuv_program, "position");
    glEnableVertexAttribArray(loc);
    glBindBuffer(GL_ARRAY_BUFFER, _quad_buffer);
    glVertexAttribPointer(loc, 2, GL_FLOAT, false, 0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(loc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!_realtime) {
    glBindTexture(GL_TEXTURE_2D, _yuv_fb_tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, _screen_data.get());
  }
  if (to_window) {
    _window.display();
  }
}

const uint8_t* Director::get_screen_data() const
{
  return _screen_data.get();
}

const trance_pb::Program& Director::program() const
{
  return *_program;
}

bool Director::vr_enabled() const
{
  return _oculus.enabled;
}

uint32_t Director::view_width() const
{
  return _oculus.enabled ? _width / 2 : _width;
}

sf::Vector2f Director::resolution() const
{
  return {float(_width), float(_height)};
}

sf::Vector2f Director::off3d(float multiplier, bool text) const
{
  float x = !_oculus.enabled || !multiplier
      ? 0.f
      : !_oculus.rendering_right ? _width / (8.f * multiplier) : _width / -(8.f * multiplier);
  x *= (text ? _system.oculus_text_depth() : _system.oculus_image_depth());
  return {x, 0};
}

void Director::render_image(const Image& image, float alpha, float multiplier, float zoom) const
{
  glEnable(GL_BLEND);
  glDisable(GL_TEXTURE_2D);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(_image_program);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, image.texture());
  glUniform1f(glGetUniformLocation(_image_program, "alpha"), alpha);
  glUniform1f(glGetUniformLocation(_image_program, "zoom"), _program->zoom_intensity() * zoom);

  GLuint ploc = glGetAttribLocation(_image_program, "position");
  glEnableVertexAttribArray(ploc);
  glBindBuffer(GL_ARRAY_BUFFER, _quad_buffer);
  glVertexAttribPointer(ploc, 2, GL_FLOAT, false, 0, 0);

  GLuint tloc = glGetAttribLocation(_image_program, "texcoord");
  glEnableVertexAttribArray(tloc);
  glBindBuffer(GL_ARRAY_BUFFER, _tex_buffer);
  glVertexAttribPointer(tloc, 2, GL_FLOAT, false, 0, 0);

  float offx3d = off3d(multiplier, false).x;
  auto x = float(image.width());
  auto y = float(image.height());

  auto scale = std::min(float(_height) / y, float(_width) / x);
  if (_oculus.enabled) {
    scale *= 0.5f;
  }
  x *= scale;
  y *= scale;

  for (int i = 0; _width / 2 - i * x + x / 2 >= 0; ++i) {
    for (int j = 0; _height / 2 - j * y + y / 2 >= 0; ++j) {
      auto x1 = offx3d + _width / 2 - x / 2;
      auto x2 = offx3d + _width / 2 + x / 2;
      auto y1 = _height / 2 - y / 2;
      auto y2 = _height / 2 + y / 2;
      render_texture(x1 - i * x, y1 - j * y, x2 - i * x, y2 - j * y, i % 2 != 0, j % 2 != 0);
      if (i != 0) {
        render_texture(x1 + i * x, y1 - j * y, x2 + i * x, y2 - j * y, i % 2 != 0, j % 2 != 0);
      }
      if (j != 0) {
        render_texture(x1 - i * x, y1 + j * y, x2 - i * x, y2 + j * y, i % 2 != 0, j % 2 != 0);
      }
      if (i != 0 && j != 0) {
        render_texture(x1 + i * x, y1 + j * y, x2 + i * x, y2 + j * y, i % 2 != 0, j % 2 != 0);
      }
    }
  }

  glDisableVertexAttribArray(ploc);
  glDisableVertexAttribArray(tloc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Director::render_text(const std::string& text, const Font& font, const sf::Color& colour,
                           const sf::Vector2f& offset, float scale) const
{
  if (text.empty()) {
    return;
  }

  struct vertex {
    float x;
    float y;
    float u;
    float v;
  };
  static std::vector<vertex> vertices;
  vertices.clear();

  auto hspace = font.font->getGlyph(' ', font.key.char_size, false).advance;
  auto vspace = font.font->getLineSpacing(font.key.char_size);
  float x = 0.f;
  float y = 0.f;
  const sf::Texture& texture = font.font->getTexture(font.key.char_size);

  float xmin = 256.f;
  float ymin = 256.f;
  float xmax = -256.f;
  float ymax = -256.f;

  uint32_t prev = 0;
  for (std::size_t i = 0; i < text.length(); ++i) {
    uint32_t current = text[i];
    x += font.font->getKerning(prev, current, font.key.char_size);
    prev = current;

    switch (current) {
    case L' ':
      x += hspace;
      continue;
    case L'\t':
      x += hspace * 4;
      continue;
    case L'\n':
      y += vspace;
      x = 0;
      continue;
    case L'\v':
      y += vspace * 4;
      continue;
    }

    const auto& g = font.font->getGlyph(current, font.key.char_size, false);
    float x1 = (x + g.bounds.left) / _width;
    float y1 = (y + g.bounds.top) / _height;
    float x2 = (x + g.bounds.left + g.bounds.width) / _width;
    float y2 = (y + g.bounds.top + g.bounds.height) / _height;
    float u1 = float(g.textureRect.left) / texture.getSize().x;
    float v1 = float(g.textureRect.top) / texture.getSize().y;
    float u2 = float(g.textureRect.left + g.textureRect.width) / texture.getSize().x;
    float v2 = float(g.textureRect.top + g.textureRect.height) / texture.getSize().y;

    vertices.push_back({x1, y1, u1, v1});
    vertices.push_back({x2, y1, u2, v1});
    vertices.push_back({x2, y2, u2, v2});
    vertices.push_back({x1, y2, u1, v2});
    xmin = std::min(xmin, std::min(x1, x2));
    xmax = std::max(xmax, std::max(x1, x2));
    ymin = std::min(ymin, std::min(y1, y2));
    ymax = std::max(ymax, std::max(y1, y2));
    x += g.advance;
  }
  for (auto& v : vertices) {
    v.x -= xmin + (xmax - xmin) / 2;
    v.y -= ymin + (ymax - ymin) / 2;
    v.x *= scale;
    v.y *= scale;
    v.x += offset.x / _width;
    v.y += offset.y / _height;
  }

  glEnable(GL_BLEND);
  glDisable(GL_TEXTURE_2D);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(_text_program);

  glActiveTexture(GL_TEXTURE0);
  sf::Texture::bind(&texture);
  glUniform4f(glGetUniformLocation(_text_program, "colour"), colour.r / 255.f, colour.g / 255.f,
              colour.b / 255.f, colour.a / 255.f);
  const char* data = reinterpret_cast<const char*>(vertices.data());

  GLuint ploc = glGetAttribLocation(_image_program, "position");
  glEnableVertexAttribArray(ploc);
  glVertexAttribPointer(ploc, 2, GL_FLOAT, false, sizeof(vertex), data);

  GLuint tloc = glGetAttribLocation(_image_program, "texcoord");
  glEnableVertexAttribArray(tloc);
  glVertexAttribPointer(tloc, 2, GL_FLOAT, false, sizeof(vertex), data + 8);
  glDrawArrays(GL_QUADS, 0, (GLsizei) vertices.size());
}

void Director::render_spiral(float spiral, uint32_t spiral_width, uint32_t spiral_type) const
{
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_CULL_FACE);

  glUseProgram(_spiral_program);
  glUniform1f(glGetUniformLocation(_spiral_program, "time"), spiral);
  glUniform2f(glGetUniformLocation(_spiral_program, "resolution"), float(view_width()),
              float(_height));

  float offset = off3d(0.f, false).x + (_oculus.rendering_right ? float(view_width()) : 0.f);
  glUniform1f(glGetUniformLocation(_spiral_program, "offset"), _oculus.enabled ? offset : 0.f);
  glUniform1f(glGetUniformLocation(_spiral_program, "width"), float(spiral_width));
  glUniform1f(glGetUniformLocation(_spiral_program, "spiral_type"), float(spiral_type));
  glUniform4f(glGetUniformLocation(_spiral_program, "acolour"), _program->spiral_colour_a().r(),
              _program->spiral_colour_a().g(), _program->spiral_colour_a().b(),
              _program->spiral_colour_a().a());
  glUniform4f(glGetUniformLocation(_spiral_program, "bcolour"), _program->spiral_colour_b().r(),
              _program->spiral_colour_b().g(), _program->spiral_colour_b().b(),
              _program->spiral_colour_b().a());

  auto loc = glGetAttribLocation(_spiral_program, "position");
  glEnableVertexAttribArray(loc);
  glBindBuffer(GL_ARRAY_BUFFER, _quad_buffer);
  glVertexAttribPointer(loc, 2, GL_FLOAT, false, 0, 0);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(loc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool Director::init_framebuffer(uint32_t& fbo, uint32_t& fb_tex, uint32_t width,
                                uint32_t height) const
{
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &fb_tex);

  glBindTexture(GL_TEXTURE_2D, fb_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);

  glBindTexture(GL_TEXTURE_2D, fb_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::cerr << "framebuffer failed" << std::endl;
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

bool Director::init_oculus_rift()
{
  if (ovr_Create(&_oculus.session, &_oculus.luid) != ovrSuccess) {
    std::cerr << "Oculus session failed" << std::endl;
    return false;
  }
  _oculus.started = false;
  auto desc = ovr_GetHmdDesc(_oculus.session);
  ovr_SetBool(_oculus.session, "QueueAheadEnabled", ovrFalse);

  ovrSizei eye_left =
      ovr_GetFovTextureSize(_oculus.session, ovrEyeType(0), desc.DefaultEyeFov[0], 1.0);
  ovrSizei eye_right =
      ovr_GetFovTextureSize(_oculus.session, ovrEyeType(1), desc.DefaultEyeFov[0], 1.0);
  int fw = eye_left.w + eye_right.w;
  int fh = std::max(eye_left.h, eye_right.h);

  ovrTextureSwapChainDesc texture_chain_desc;
  texture_chain_desc.Type = ovrTexture_2D;
  texture_chain_desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
  texture_chain_desc.ArraySize = 1;
  texture_chain_desc.Width = fw;
  texture_chain_desc.Height = fh;
  texture_chain_desc.MipLevels = 0;
  texture_chain_desc.SampleCount = 1;
  texture_chain_desc.StaticImage = false;
  texture_chain_desc.MiscFlags = ovrTextureMisc_None;
  texture_chain_desc.BindFlags = 0;

  auto result =
      ovr_CreateTextureSwapChainGL(_oculus.session, &texture_chain_desc, &_oculus.texture_chain);
  if (result != ovrSuccess) {
    std::cerr << "Oculus texture swap chain failed" << std::endl;
    ovrErrorInfo info;
    ovr_GetLastErrorInfo(&info);
    std::cerr << info.ErrorString << std::endl;
  }
  int texture_count = 0;
  result = ovr_GetTextureSwapChainLength(_oculus.session, _oculus.texture_chain, &texture_count);
  if (result != ovrSuccess) {
    std::cerr << "Oculus texture swap chain length failed" << std::endl;
  }
  for (int i = 0; i < texture_count; ++i) {
    GLuint fbo;
    GLuint fb_tex = 0;
    result = ovr_GetTextureSwapChainBufferGL(_oculus.session, _oculus.texture_chain, i, &fb_tex);
    if (result != ovrSuccess) {
      std::cerr << "Oculus texture swap chain buffer failed" << std::endl;
    }

    glGenFramebuffers(1, &fbo);
    _oculus.fbo_ovr.push_back(fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindTexture(GL_TEXTURE_2D, fb_tex);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      std::cerr << "framebuffer failed" << std::endl;
      return false;
    }
  }

  auto erd_left = ovr_GetRenderDesc(_oculus.session, ovrEye_Left, desc.DefaultEyeFov[0]);
  auto erd_right = ovr_GetRenderDesc(_oculus.session, ovrEye_Right, desc.DefaultEyeFov[1]);
  _oculus.eye_view_offset[0] = erd_left.HmdToEyeOffset;
  _oculus.eye_view_offset[1] = erd_right.HmdToEyeOffset;

  _oculus.layer.Header.Type = ovrLayerType_EyeFov;
  _oculus.layer.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;
  _oculus.layer.ColorTexture[0] = _oculus.texture_chain;
  _oculus.layer.ColorTexture[1] = _oculus.texture_chain;
  _oculus.layer.Fov[0] = erd_left.Fov;
  _oculus.layer.Fov[1] = erd_right.Fov;
  _oculus.layer.Viewport[0].Pos.x = 0;
  _oculus.layer.Viewport[0].Pos.y = 0;
  _oculus.layer.Viewport[0].Size.w = fw / 2;
  _oculus.layer.Viewport[0].Size.h = fh;
  _oculus.layer.Viewport[1].Pos.x = fw / 2;
  _oculus.layer.Viewport[1].Pos.y = 0;
  _oculus.layer.Viewport[1].Size.w = fw / 2;
  _oculus.layer.Viewport[1].Size.h = fh;

  _width = fw;
  _height = fh;
  _window.setSize(sf::Vector2u(0, 0));
  return true;
}

void Director::change_visual(uint32_t length)
{
  // Like !random_chance(chance), but scaled to current speed and cycle length.
  // Roughly 1/2 chance for a cycle of length 2048.
  auto fps = program().global_fps();
  if (length && random((2 * fps * length) / 2048) >= 120) {
    return;
  }
  _visual.swap(_old_visual);

  uint32_t total = 0;
  for (const auto& type : _program->visual_type()) {
    total += type.random_weight();
  }
  auto r = random(total);
  total = 0;
  trance_pb::Program_VisualType t;
  for (const auto& type : _program->visual_type()) {
    if (r < (total += type.random_weight())) {
      t = type.type();
      break;
    }
  }

  // TODO: if it's the same as the last choice, don't reset!
  if (t == trance_pb::Program_VisualType_ACCELERATE) {
    _visual.reset(new AccelerateVisual{*_visual_api});
  }
  if (t == trance_pb::Program_VisualType_SLOW_FLASH) {
    _visual.reset(new SlowFlashVisual{*_visual_api});
  }
  if (t == trance_pb::Program_VisualType_SUB_TEXT) {
    _visual.reset(new SubTextVisual{*_visual_api});
  }
  if (t == trance_pb::Program_VisualType_FLASH_TEXT) {
    _visual.reset(new FlashTextVisual{*_visual_api});
  }
  if (t == trance_pb::Program_VisualType_PARALLEL) {
    _visual.reset(new ParallelVisual{*_visual_api});
  }
  if (t == trance_pb::Program_VisualType_SUPER_PARALLEL) {
    _visual.reset(new SuperParallelVisual{*_visual_api});
  }
  if (t == trance_pb::Program_VisualType_ANIMATION) {
    _visual.reset(new AnimationVisual{*_visual_api});
  }
  if (t == trance_pb::Program_VisualType_SUPER_FAST) {
    _visual.reset(new SuperFastVisual{*_visual_api});
  }
}

void Director::render_texture(float l, float t, float r, float b, bool flip_h, bool flip_v) const
{
  glUniform2f(glGetUniformLocation(_image_program, "min_coord"), l / _width, t / _height);
  glUniform2f(glGetUniformLocation(_image_program, "max_coord"), r / _width, b / _height);
  glUniform2f(glGetUniformLocation(_image_program, "flip"), flip_h ? 1.f : 0.f, flip_v ? 1.f : 0.f);
  glDrawArrays(GL_TRIANGLES, 0, 6);
}
