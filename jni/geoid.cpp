#include <android_native_app_glue.h>

#include <errno.h>
#include <jni.h>
#include <sys/time.h>
#include <time.h>
#include <android/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <queue>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#define  LOG_TAG    "geoid_native"
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#define  LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define  LOGW(...)  __android_log_print(ANDROID_LOG_WARN,LOG_TAG,__VA_ARGS__)
#define  LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)

struct engine {
	android_app* app;
	cv::Ptr<cv::VideoCapture> capture;

	bool hasFocus;

	bool grabFeatures;
	bool trained;

	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	int32_t width;
	int32_t height;

	GLuint gProgram;
	GLuint gvPositionHandle;
};

static void printGLString(const char *name, GLenum s) {
	const char *v = (const char *) glGetString(s);
	LOGI("GL %s = %s\n", name, v);
}

static void checkGlError(const char* op) {
	for (GLint error = glGetError(); error; error = glGetError()) {
		LOGI("after %s() glError (0x%x)\n", op, error);
	}
}

static const char gVertexShader[] = "attribute vec4 vPosition;\n"
		"void main() {\n"
		"  gl_Position = vPosition;\n"
		"}\n";

static const char gFragmentShader[] = "precision mediump float;\n"
		"void main() {\n"
		"  gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
		"}\n";

GLuint loadShader(GLenum shaderType, const char* pSource) {
	GLuint shader = glCreateShader(shaderType);
	if (shader) {
		glShaderSource(shader, 1, &pSource, NULL);
		glCompileShader(shader);
		GLint compiled = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (!compiled) {
			GLint infoLen = 0;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
			if (infoLen) {
				char* buf = (char*) malloc(infoLen);
				if (buf) {
					glGetShaderInfoLog(shader, infoLen, NULL, buf);
					LOGE("Could not compile shader %d:\n%s\n", shaderType, buf);
					free(buf);
				}
				glDeleteShader(shader);
				shader = 0;
			}
		}
	}
	return shader;
}

GLuint createProgram(const char* pVertexSource, const char* pFragmentSource) {
	GLuint vertexShader = loadShader(GL_VERTEX_SHADER, pVertexSource);
	if (!vertexShader) {
		return 0;
	}

	GLuint pixelShader = loadShader(GL_FRAGMENT_SHADER, pFragmentSource);
	if (!pixelShader) {
		return 0;
	}

	GLuint program = glCreateProgram();
	if (program) {
		glAttachShader(program, vertexShader);
		checkGlError("glAttachShader");
		glAttachShader(program, pixelShader);
		checkGlError("glAttachShader");
		glLinkProgram(program);
		GLint linkStatus = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
		if (linkStatus != GL_TRUE) {
			GLint bufLength = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
			if (bufLength) {
				char* buf = (char*) malloc(bufLength);
				if (buf) {
					glGetProgramInfoLog(program, bufLength, NULL, buf);
					LOGE("Could not link program:\n%s\n", buf);
					free(buf);
				}
			}
			glDeleteProgram(program);
			program = 0;
		}
	}
	return program;
}

bool setupGraphics(struct engine* engine) {
	printGLString("Version", GL_VERSION);
	printGLString("Vendor", GL_VENDOR);
	printGLString("Renderer", GL_RENDERER);
	printGLString("Extensions", GL_EXTENSIONS);

	int w = engine->width;
	int h = engine->height;

	LOGI("setupGraphics(%d, %d)", w, h);
	engine->gProgram = createProgram(gVertexShader, gFragmentShader);
	if (!engine->gProgram) {
		LOGE("Could not create program.");
		return false;
	}
	engine->gvPositionHandle = glGetAttribLocation(engine->gProgram,
			"vPosition");
	checkGlError("glGetAttribLocation");
	LOGI("glGetAttribLocation(\"vPosition\") = %d\n", engine->gvPositionHandle);

	glViewport(0, 0, w, h);
	checkGlError("glViewport");
	return true;
}

const GLfloat gTriangleVertices[] = { 0.0f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f };

void renderFrame(struct engine* engine) {
	static float grey;
	grey += 0.01f;
	if (grey > 1.0f) {
		grey = 0.0f;
	}
    glClearColor(grey, grey, grey, 1.0f);
    checkGlError("glClearColor");
    glClear( GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    checkGlError("glClear");

	glUseProgram(engine->gProgram);
	checkGlError("glUseProgram");

	glVertexAttribPointer(engine->gvPositionHandle, 2, GL_FLOAT, GL_FALSE, 0,
			gTriangleVertices);
	checkGlError("glVertexAttribPointer");
	glEnableVertexAttribArray(engine->gvPositionHandle);
	checkGlError("glEnableVertexAttribArray");
	glDrawArrays(GL_TRIANGLES, 0, 3);
	checkGlError("glDrawArrays");

	eglSwapBuffers(engine->display, engine->surface);
}

/**
 * Initialize an EGL context for the current display.
 */
static int engine_init_display(struct engine* engine) {
	// initialize OpenGL ES and EGL

	/*
	 * Here specify the attributes of the desired configuration.
	 * Below, we select an EGLConfig with at least 8 bits per color
	 * component compatible with on-screen windows
	 */
	const EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE,
			8, EGL_RED_SIZE, 8, EGL_NONE };
	EGLint w, h, dummy, format;
	EGLint numConfigs;
	EGLConfig config;
	EGLSurface surface;
	EGLContext context;

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

	eglInitialize(display, 0, 0);

	/* Here, the application chooses the configuration it desires. In this
	 * sample, we have a very simplified selection process, where we pick
	 * the first EGLConfig that matches our criteria */
	eglChooseConfig(display, attribs, &config, 1, &numConfigs);

	/* EGL_NATIVE_VISUAL_ID is an attribute of the EGLConfig that is
	 * guaranteed to be accepted by ANativeWindow_setBuffersGeometry().
	 * As soon as we picked a EGLConfig, we can safely reconfigure the
	 * ANativeWindow buffers to match, using EGL_NATIVE_VISUAL_ID. */
	eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);

	ANativeWindow_setBuffersGeometry(engine->app->window, 0, 0, format);

	surface = eglCreateWindowSurface(display, config, engine->app->window,
			NULL);

	EGLint AttribList[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	context = eglCreateContext(display, config, NULL, AttribList);

	if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
		LOGW("Unable to eglMakeCurrent");
		return -1;
	}

	eglQuerySurface(display, surface, EGL_WIDTH, &w);
	eglQuerySurface(display, surface, EGL_HEIGHT, &h);

	engine->display = display;
	engine->context = context;
	engine->surface = surface;
	engine->width = w;
	engine->height = h;

	// Initialize GL state.
	glEnable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);

	return 0;
}

/**
 * Tear down the EGL context currently associated with the display.
 */
static void engine_term_display(struct engine* engine) {
	if (engine->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
				EGL_NO_CONTEXT);
		if (engine->context != EGL_NO_CONTEXT) {
			eglDestroyContext(engine->display, engine->context);
		}
		if (engine->surface != EGL_NO_SURFACE) {
			eglDestroySurface(engine->display, engine->surface);
		}
		eglTerminate(engine->display);
	}
	engine->hasFocus = false;
	engine->display = EGL_NO_DISPLAY;
	engine->context = EGL_NO_CONTEXT;
	engine->surface = EGL_NO_SURFACE;
}

static cv::Size calc_optimal_camera_resolution(const char* supported, int width,
		int height) {
	int frame_width = 0;
	int frame_height = 0;

	size_t prev_idx = 0;
	size_t idx = 0;
	float min_diff = FLT_MAX;

	do {
		int tmp_width;
		int tmp_height;

		prev_idx = idx;
		while ((supported[idx] != '\0') && (supported[idx] != ','))
			idx++;

		sscanf(&supported[prev_idx], "%dx%d", &tmp_width, &tmp_height);

		int w_diff = width - tmp_width;
		int h_diff = height - tmp_height;
		if ((h_diff >= 0) && (w_diff >= 0)) {
			if ((h_diff <= min_diff) && (tmp_height <= 720)) {
				frame_width = tmp_width;
				frame_height = tmp_height;
				min_diff = h_diff;
			}
		}

		idx++; // to skip coma symbol

	} while (supported[idx - 1] != '\0');

	return cv::Size(frame_width, frame_height);
}

static void engine_draw_frame(engine* engine, const cv::Mat& frame) {
	if (engine->app->window == NULL)
		return; // No window.

//	ANativeWindow_Buffer buffer;
//	if (ANativeWindow_lock(engine->app->window, &buffer, NULL) < 0) {
//		LOGW("Unable to lock window buffer");
//		return;
//	}
//
//	void* pixels = buffer.bits;
//
//	int left_indent = (buffer.width - frame.cols) / 2;
//	int top_indent = (buffer.height - frame.rows) / 2;
//
//	for (int yy = top_indent;
//			yy < std::min(frame.rows + top_indent, buffer.height); yy++) {
//		unsigned char* line = (unsigned char*) pixels;
//		memcpy(line + left_indent * 4 * sizeof(unsigned char),
//				frame.ptr<unsigned char>(yy),
//				std::min(frame.cols, buffer.width) * 4 * sizeof(unsigned char));
//		// go to next line
//		pixels = (int32_t*) pixels + buffer.stride;
//	}
//	ANativeWindow_unlockAndPost(engine->app->window);

	// then render opengl frame
	renderFrame(engine);
}

/**
 * Process the next input event.
 */
static int32_t engine_handle_input(struct android_app* app,
		AInputEvent* event) {
	struct engine* engine = (struct engine*) app->userData;
	if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
		// grab next frame's features
		engine->grabFeatures = true;
		return 1;
	}
	return 0;
}

static void engine_handle_cmd(android_app* app, int32_t cmd) {
	struct engine* engine = (struct engine*) app->userData;
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		if (app->window != NULL) {
			LOGI("APP_CMD_INIT_WINDOW");

			engine->capture = new cv::VideoCapture(0);

			union {
				double prop;
				const char* name;
			} u;
			u.prop = engine->capture->get(
					CV_CAP_PROP_SUPPORTED_PREVIEW_SIZES_STRING);

			int view_width = ANativeWindow_getWidth(app->window);
			int view_height = ANativeWindow_getHeight(app->window);

			cv::Size camera_resolution;
			if (u.name)
				camera_resolution = calc_optimal_camera_resolution(u.name, 640,
						480);
			else {
				LOGE("Cannot get supported camera camera_resolutions");
				camera_resolution = cv::Size(
						ANativeWindow_getWidth(app->window),
						ANativeWindow_getHeight(app->window));
			}

			if ((camera_resolution.width != 0)
					&& (camera_resolution.height != 0)) {
				engine->capture->set(CV_CAP_PROP_FRAME_WIDTH,
						camera_resolution.width);
				engine->capture->set(CV_CAP_PROP_FRAME_HEIGHT,
						camera_resolution.height);
			}

			float scale = std::min((float) view_width / camera_resolution.width,
					(float) view_height / camera_resolution.height);

			if (ANativeWindow_setBuffersGeometry(app->window,
					(int) (view_width / scale), int(view_height / scale),
					WINDOW_FORMAT_RGBA_8888) < 0) {
				LOGE("Cannot set pixel format!");
				return;
			}

			LOGI(
					"Camera initialized at resoution %dx%d", camera_resolution.width, camera_resolution.height);

			engine_init_display(engine);
			setupGraphics(engine);
		}
		break;
	case APP_CMD_GAINED_FOCUS:
		// start capturing frames
		engine->hasFocus = true;
		break;
	case APP_CMD_LOST_FOCUS:
		// stop capturing frames
		engine->hasFocus = false;
		break;
	case APP_CMD_TERM_WINDOW:
		LOGI("APP_CMD_TERM_WINDOW");

		engine_term_display(engine);
		engine->capture->release();
		break;
	}
}

void android_main(android_app* app) {
	engine engine;

	// Make sure glue isn't stripped.
	app_dummy();

	memset(&engine, 0, sizeof(engine));
	app->userData = &engine;
	app->onAppCmd = engine_handle_cmd;
	app->onInputEvent = engine_handle_input;
	engine.app = app;

	float fps = 0;
	cv::Mat drawing_frame;
	std::queue<int64> time_queue;

	cv::ORB orb;
	cv::Mat keypoints;
	cv::Mat trainDescriptors, queryDescriptors;
	cv::Mat mask; // empty

	cv::Scalar textColor(0, 255, 0, 255), keypointColor(255, 255, 255);

	cv::BFMatcher matcher;

	engine.grabFeatures = false;
	engine.trained = false;

	// loop waiting for stuff to do.
	while (1) {
		// Read all pending events.
		int ident;
		int events;
		android_poll_source* source;

		// Process system events
		// if we don't have focus, block forever, otherwise capture continuously
		while ((ident = ALooper_pollAll(engine.hasFocus ? 0 : -1, NULL, &events,
				(void**) &source)) >= 0) {
			// Process this event.
			if (source != NULL) {
				source->process(app, source);
			}

			// Check if we are exiting.
			if (app->destroyRequested != 0) {
				LOGI("Engine thread destroy requested!");
				engine_term_display(&engine);
				return;
			}
		}

		engine_draw_frame(&engine, drawing_frame);

//		int64 then;
//		int64 now = cv::getTickCount();
//		time_queue.push(now);
//
//		// Capture frame from camera and draw it
//		if (engine.hasFocus && !engine.capture.empty()) {
//			if (engine.capture->grab())
//				engine.capture->retrieve(drawing_frame,
//						CV_CAP_ANDROID_COLOR_FRAME_RGBA);
//
//			cv::cvtColor(drawing_frame, drawing_frame, cv::COLOR_RGBA2RGB);
//
//			// my stuff
//			std::vector<cv::KeyPoint> keypoints;
//
//			if (engine.grabFeatures) {
//				orb(drawing_frame, mask, keypoints, trainDescriptors);
//
//				engine.grabFeatures = false;
//				engine.trained = true;
//
//				LOGI("got training descriptors!");
//
//				// show keypoints
//				cv::drawKeypoints(drawing_frame, keypoints, drawing_frame);
//			} else if (engine.trained) {
//				orb(drawing_frame, mask, keypoints, queryDescriptors);
//
//				std::vector<cv::DMatch> matches;
//				matcher.match(queryDescriptors, trainDescriptors, matches);
//
//				float min = INFINITY;
//				for (int i = 0; i < matches.size(); ++i) {
//					if (matches[i].distance < min) {
//						min = matches[i].distance;
//					}
//				}
//
//				float threshold = 2 * min;
//
//				for (int i = 0; i < matches.size(); ++i) {
//					cv::KeyPoint keypoint = keypoints[matches[i].queryIdx];
//					if (matches[i].distance < threshold) {
//						char buf[4];
//						sprintf(buf, "%d", matches[i].trainIdx);
//						cv::putText(drawing_frame, std::string(buf),
//								keypoint.pt, cv::FONT_HERSHEY_PLAIN, 1,
//								keypointColor);
//					}
//				}
//			} else {
//				// show keypoints
//				cv::drawKeypoints(drawing_frame, keypoints, drawing_frame);
//			}
//
//			char buffer[256];
//			sprintf(buffer, "Display performance: %dx%d @ %.3f",
//					drawing_frame.cols, drawing_frame.rows, fps);
//			cv::putText(drawing_frame, std::string(buffer), cv::Point(8, 64),
//					cv::FONT_HERSHEY_COMPLEX_SMALL, 1, textColor);
//
//			cv::cvtColor(drawing_frame, drawing_frame, cv::COLOR_RGB2RGBA);
//			engine_draw_frame(&engine, drawing_frame);
//		}
//
//		if (time_queue.size() >= 2)
//			then = time_queue.front();
//		else
//			then = 0;
//
//		if (time_queue.size() >= 25)
//			time_queue.pop();
//
//		fps = time_queue.size() * (float) cv::getTickFrequency() / (now - then);
	}
}