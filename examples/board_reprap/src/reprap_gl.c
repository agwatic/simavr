/*
	reprap_gl.c

	Copyright 2008-2012 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#if __APPLE__
#define GL_GLEXT_PROTOTYPES
#include <GLUT/glut.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glut.h>
#include <GL/glext.h>
#endif

#include <stdio.h>
#include <math.h>

#include "reprap.h"
#include "reprap_gl.h"

#include "c3.h"
#include "c3camera.h"
#include "c3driver_context.h"
#include "c3stl.h"
#include "c3lines.h"

#include <cairo/cairo.h>

struct cairo_surface_t;

int _w = 800, _h = 600;
//c3cam cam;
c3context_p c3;
c3context_p hud;
c3object_p head;
c3texture_p fbo_c3;

int glsl_version = 110;

extern reprap_t reprap;

static int dumpError(const char * what)
{
	GLenum e;
	int count = 0;
	while ((e = glGetError()) != GL_NO_ERROR) {
		printf("%s: %s\n", what, gluErrorString(e));
		count++;
	}
	return count;
}

#define GLCHECK(_w) {_w; dumpError(#_w);}

void print_log(GLuint obj)
{
	int infologLength = 0;
	int maxLength;

	if(glIsShader(obj))
		glGetShaderiv(obj,GL_INFO_LOG_LENGTH,&maxLength);
	else
		glGetProgramiv(obj,GL_INFO_LOG_LENGTH,&maxLength);

	char infoLog[maxLength];

	if (glIsShader(obj))
		glGetShaderInfoLog(obj, maxLength, &infologLength, infoLog);
	else
		glGetProgramInfoLog(obj, maxLength, &infologLength, infoLog);

	if (infologLength > 0)
		printf("%s\n",infoLog);
}
/* Global */
GLuint fbo, fbo_texture, rbo_depth;
//GLuint vbo_fbo_vertices;

static void
gl_offscreenInit(
		int screen_width,
		int screen_height)
{
	/* init_resources */
	/* Create back-buffer, used for post-processing */

	/* Texture */
	GLCHECK(glActiveTexture(GL_TEXTURE0));
	glGenTextures(1, &fbo_texture);
	glBindTexture(GL_TEXTURE_2D, fbo_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_width, screen_height, 0,
	        GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* Depth buffer */
	GLCHECK(glGenRenderbuffers(1, &rbo_depth));
	glBindRenderbuffer(GL_RENDERBUFFER, rbo_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, screen_width,
	        screen_height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	/* Framebuffer to link everything together */
	GLCHECK(glGenFramebuffers(1, &fbo));
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	        fbo_texture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	        GL_RENDERBUFFER, rbo_depth);

	GLenum status;
	if ((status = glCheckFramebufferStatus(GL_FRAMEBUFFER))
	        != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "glCheckFramebufferStatus: error %d", (int)status);
		return ;
	}
#if 0
	// Set the list of draw buffers.
	GLenum DrawBuffers[2] = {GL_COLOR_ATTACHMENT0};
	glDrawBuffers(1, DrawBuffers); // "1" is the size of DrawBuffers
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

void
gl_offscreenReshape(
		int screen_width,
		int screen_height)
{
// Rescale FBO and RBO as well
	glBindTexture(GL_TEXTURE_2D, fbo_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, screen_width, screen_height, 0,
	        GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindRenderbuffer(GL_RENDERBUFFER, rbo_depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, screen_width,
	        screen_height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

//	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
//    glViewport(0, 0, screen_width, screen_height);
//	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_offscreenFree()
{
	/* free_resources */
	glDeleteRenderbuffers(1, &rbo_depth);
	glDeleteTextures(1, &fbo_texture);
	glDeleteFramebuffers(1, &fbo);
//	  glDeleteBuffers(1, &vbo_fbo_vertices);
}

GLuint program_postproc = 0, uniform_fbo_texture;

static GLuint create_shader(const char * fname, GLuint pid)
{
	const GLchar * buf;

	FILE *f = fopen(fname, "r");
	if (!f) {
		perror(fname);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long fs = ftell(f);
	fseek(f, 0, SEEK_SET);
	/*
	 * need to insert a header since there is nothing to detect the version number
	 * reliably without it, and __VERSION__ returns idiocy
	 */
	char head[128];
	sprintf(head, "#version %d\n#define GLSL_VERSION %d\n", glsl_version, glsl_version);
	const int header = strlen(head);
	buf = malloc(header + fs + 1);
	memcpy((void*)buf, head, header);
	fread((void*)buf + header, 1, fs, f);
	((char*)buf)[header + fs] = 0;
	fclose(f);

	GLuint vs = glCreateShader(pid);
	glShaderSource(vs, 1, &buf, NULL);
	glCompileShader(vs);
	dumpError("glCompileShader");
	print_log(vs);
	free((void*)buf);
	return vs;
}


int gl_ppProgram()
{
	int vs, fs;
	int link_ok, validate_ok;
	/* init_resources */
	/* Post-processing */
	if ((vs = create_shader("gfx/postproc.vs", GL_VERTEX_SHADER)) == 0)
		return 0;
	if ((fs = create_shader("gfx/postproc.fs", GL_FRAGMENT_SHADER)) == 0)
		return 0;

	program_postproc = glCreateProgram();
	glAttachShader(program_postproc, vs);
	glAttachShader(program_postproc, fs);
	glLinkProgram(program_postproc);
	glGetProgramiv(program_postproc, GL_LINK_STATUS, &link_ok);
	if (!link_ok) {
		fprintf(stderr, "glLinkProgram:");
		goto error;
	}
	glValidateProgram(program_postproc);
	glGetProgramiv(program_postproc, GL_VALIDATE_STATUS, &validate_ok);
	if (!validate_ok) {
		fprintf(stderr, "glValidateProgram:");
		goto error;
	}

	char * uniform_name = "m_Texture";
	uniform_fbo_texture = glGetUniformLocation(program_postproc, uniform_name);
	if (uniform_fbo_texture == -1) {
		fprintf(stderr, "Could not bind uniform %s\n", uniform_name);
		goto error;
	}
	return 0;
error:
	print_log(program_postproc);
	glDeleteProgram(program_postproc);
	program_postproc = 0;
	return -1;
}

void
gl_ppFree()
{
	if (program_postproc)
		glDeleteProgram(program_postproc);
	program_postproc = 0;
}

static void
_gl_reshape_cb(int w, int h)
{
    _w  = w;
    _h = h;

    glViewport(0, 0, _w, _h);
    gl_offscreenReshape(_w, _h);
    glutPostRedisplay();
}

static void
_gl_key_cb(
		unsigned char key,
		int x,
		int y)	/* called on key press */
{
	switch (key) {
		case 'q':
		//	avr_vcd_stop(&vcd_file);
			c3context_dispose(c3);
			exit(0);
			break;
		case 'r':
			printf("Starting VCD trace; press 's' to stop\n");
		//	avr_vcd_start(&vcd_file);
			break;
		case 's':
			printf("Stopping VCD trace\n");
		//	avr_vcd_stop(&vcd_file);
			break;
		case '1':
			if (fbo_c3->geometry.mat.program.pid)
				fbo_c3->geometry.mat.program.pid = 0;
			else
				fbo_c3->geometry.mat.program.pid = program_postproc;
			glutPostRedisplay();
			break;
	}
}

static void
_c3_load_pixels(
		c3pixels_p pix)
{
	GLuint mode = pix->normalize ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE_ARB;
	if (!pix->texture) {
		printf("Creating texture %s %dx%d\n", pix->name ? pix->name->str : "", pix->w, pix->h);
		pix->dirty = 1;
		GLuint texID = 0;
		dumpError("cp_gl_texture_load_argb flush");
		GLCHECK(glEnable(mode));

		glGenTextures(1, &texID);
//		glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
//				GL_MODULATE); //set texture environment parameters
//		dumpError("glTexEnvf");

		glPixelStorei(GL_UNPACK_ROW_LENGTH, pix->row / pix->psize);
		glTexParameteri(mode, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		dumpError("GL_TEXTURE_MAG_FILTER");//
		glTexParameteri(mode, GL_TEXTURE_MIN_FILTER,
				pix->normalize ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		dumpError("GL_TEXTURE_MIN_FILTER");
		glTexParameteri(mode, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		dumpError("GL_TEXTURE_WRAP_S");
		glTexParameteri(mode, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		dumpError("GL_TEXTURE_WRAP_T");
		if (pix->normalize)
			GLCHECK(glTexParameteri(mode, GL_GENERATE_MIPMAP, GL_TRUE));
	#if 1
		GLfloat fLargest;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &fLargest);
		//printf("fLargest = %f\n", fLargest);
		GLCHECK(glTexParameterf(mode, GL_TEXTURE_MAX_ANISOTROPY_EXT, fLargest));
	#endif
		if (pix->normalize)
			GLCHECK(glGenerateMipmap(mode));

		pix->texture = texID;
		pix->dirty = 1;
	}
	if (pix->dirty) {
		pix->dirty = 0;
		GLCHECK(glBindTexture(mode, pix->texture));
		glTexImage2D(mode, 0,
				pix->format == C3PIXEL_A ? GL_ALPHA16 : GL_RGBA8,
				pix->w, pix->h, 0,
				pix->format == C3PIXEL_A ? GL_ALPHA : GL_BGRA,
				GL_UNSIGNED_BYTE,
				pix->base);
		dumpError("glTexImage2D");
		if (pix->normalize)
			GLCHECK(glGenerateMipmap(mode));
	}
}

static void
_c3_geometry_project(
		c3context_p c,
		const struct c3driver_context_t * d,
		c3geometry_p g,
		c3mat4p m)
{
	if (g->mat.texture) {
//		printf("_c3_geometry_project xrure %d!\n", g->textures.count);
		_c3_load_pixels(g->mat.texture);
	}

	switch(g->type.type) {
		case C3_TRIANGLE_TYPE:
			g->type.subtype = GL_TRIANGLES;
			break;
		case C3_TEXTURE_TYPE: {
		//	c3texture_p t = (c3texture_p)g;
			if (g->mat.texture) {
				g->type.subtype = GL_TRIANGLE_FAN;
			}
		}	break;
		case C3_LINES_TYPE:
			g->type.subtype = GL_TRIANGLES;
			break;
		default:
		    break;
	}
}

static void
_c3_geometry_draw(
		c3context_p c,
		const struct c3driver_context_t *d,
		c3geometry_p g )
{
	glColor4fv(g->mat.color.n);
	dumpError("glColor");
//	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, g->mat.color.n);
	glVertexPointer(3, GL_FLOAT, 0,
			g->projected.count ? g->projected.e : g->vertice.e);
	glEnableClientState(GL_VERTEX_ARRAY);
	dumpError("GL_VERTEX_ARRAY");
	glDisable(GL_TEXTURE_2D);
	if (g->mat.texture) {
		GLuint mode = g->mat.texture->normalize ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE_ARB;
		glEnable(mode);
		if (g->mat.texture->trace)
			printf("%s uses texture %s (%d tex)\n",
					__func__, g->mat.texture->name->str, g->textures.count);
	//	printf("tex mode %d texture %d\n", g->mat.mode, g->mat.texture);
		dumpError("glEnable texture");
		glBindTexture(mode, g->mat.texture->texture);
		dumpError("glBindTexture");
		glTexCoordPointer(2, GL_FLOAT, 0, g->textures.e);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		dumpError("GL_TEXTURE_COORD_ARRAY");
	}
	if (g->mat.program.pid) {
		glUseProgram(g->mat.program.pid);
		dumpError("glUseProgram program_postproc");
	}
	if (g->normals.count) {
		glNormalPointer(GL_FLOAT, 0, g->normals.e);
		glEnableClientState(GL_NORMAL_ARRAY);
	}
	glDrawArrays(g->type.subtype, 0,
			g->projected.count ? g->projected.count : g->vertice.count);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	if (g->mat.texture)
		glDisable(g->mat.texture->normalize ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE_ARB);
	if (g->mat.program.pid)
		glUseProgram(0);
}

const c3driver_context_t c3context_driver = {
		.geometry_project = _c3_geometry_project,
		.geometry_draw = _c3_geometry_draw,
};

float z_min, z_max;
/*
 * Computes the distance from the eye, sort by this value
 */
static int
_c3_z_sorter(
		const void *_p1,
		const void *_p2)
{
	c3geometry_p g1 = *(c3geometry_p*)_p1;
	c3geometry_p g2 = *(c3geometry_p*)_p2;
	// get center of bboxes
	c3vec3 c1 = c3vec3_add(g1->bbox.min, c3vec3_divf(c3vec3_sub(g1->bbox.max, g1->bbox.min), 2));
	c3vec3 c2 = c3vec3_add(g2->bbox.min, c3vec3_divf(c3vec3_sub(g2->bbox.max, g2->bbox.min), 2));

	c3f d1 = c3vec3_length2(c3vec3_sub(c1, c3->cam.eye));
	c3f d2 = c3vec3_length2(c3vec3_sub(c2, c3->cam.eye));

	if (d1 > z_max) z_max = d1;
	if (d1 < z_min) z_min = d1;
	if (d2 > z_max) z_max = d2;
	if (d2 < z_min) z_min = d2;
	/*
	 * make sure transparent items are drawn after everyone else
	 */
	if (g1->mat.color.n[3] < 1)
		d1 -= 100000.0;
	if (g2->mat.color.n[3] < 1)
		d2 -= 100000.0;

	return d1 < d2 ? 1 : d1 > d2 ? -1 : 0;
}

#define FBO 1

static void
_gl_display_cb(void)		/* function called whenever redisplay needed */
{
#if FBO
	/*
	 * Draw in FBO object
	 */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	// draw (without glutSwapBuffers)
	dumpError("glBindFramebuffer fbo");
	glViewport(0, 0, _w, _h);

#else
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif

	c3vec3 headp = c3vec3f(
			stepper_get_position_mm(&reprap.step_x),
			stepper_get_position_mm(&reprap.step_y),
			stepper_get_position_mm(&reprap.step_z));
	c3mat4 headmove = translation3D(headp);
	c3transform_set(head->transform.e[0], &headmove);

	if (c3->root->dirty) {
	//	printf("reproject head %.2f,%.2f,%.2f\n", headp.x, headp.y,headp.z);
		c3context_project(c3);

		z_min = 1000000000;
		z_max = -1000000000;
		qsort(c3->projected.e, c3->projected.count, sizeof(c3->projected.e[0]),
		        _c3_z_sorter);
		z_min = sqrt(z_min);
		z_max = sqrt(z_max);
		//	printf("z_min %f, z_max %f\n", z_min, z_max);
		//z_min -= 50;
		if (z_min < 0)
			z_min = 10;
		z_min = 10;
		if (z_max < z_min || z_max > 1000)
			z_max = 1000;
	}


	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Set up projection matrix
	glMatrixMode(GL_PROJECTION); // Select projection matrix
	glLoadIdentity(); // Start with an identity matrix

	gluPerspective(50, (float)_w / (float)_h, z_min, z_max);
#if 0
	glCullFace(GL_BACK);
	glEnable(GL_CULL_FACE);
#endif
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_LIGHTING);
//	glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 1.0f / 255.0f);
	glEnable(GL_BLEND); // Enable Blending
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Type Of Blending To Use

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glMultMatrixf(c3->cam.mtx.n);
	glTranslatef(-c3->cam.eye.n[VX], -c3->cam.eye.n[VY], -c3->cam.eye.n[VZ]);

	dumpError("flush");

	c3context_draw(c3);

#if FBO
	/*
	 * Draw back FBO over the screen
	 */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	dumpError("glBindFramebuffer 0");

	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_ALPHA_TEST);

	glUseProgram(0);

	glMatrixMode(GL_PROJECTION); // Select projection matrix
	glLoadIdentity(); // Start with an identity matrix
	glOrtho(0, _w, 0, _h, 0, 10);
	glScalef(1, -1, 1);
	glTranslatef(0, -1 * _h, 0);
	glMatrixMode(GL_MODELVIEW); // Select modelview matrix
	glLoadIdentity(); // Start with an identity matrix

	if (hud)
		c3context_draw(hud);

    glutSwapBuffers();
}

#if !defined(GLUT_WHEEL_UP)
#  define GLUT_WHEEL_UP   3
#  define GLUT_WHEEL_DOWN 4
#endif


int button;
c3vec2 move;

static
void _gl_button_cb(
		int b,
		int s,
		int x,
		int y)
{
	button = s == GLUT_DOWN ? b : 0;
	move = c3vec2f(x, y);
//	printf("button %d: %.1f,%.1f\n", b, move.x, move.y);
	switch (b) {
		case GLUT_LEFT_BUTTON:
		case GLUT_RIGHT_BUTTON:	// call motion
			break;
		case GLUT_WHEEL_UP:
		case GLUT_WHEEL_DOWN:
			if (c3->cam.distance > 10) {
				const float d = 0.004;
				c3cam_set_distance(&c3->cam, c3->cam.distance * ((b == GLUT_WHEEL_DOWN) ? (1.0+d) : (1.0-d)));
				c3cam_update_matrix(&c3->cam);
				c3->root->dirty = 1;	// resort the array
			}
			break;
	}
}

void
_gl_motion_cb(
		int x,
		int y)
{
	c3vec2 m = c3vec2f(x, y);
	c3vec2 delta = c3vec2_sub(move, m);

//	printf("%s b%d click %.1f,%.1f now %d,%d delta %.1f,%.1f\n",
//			__func__, button, move.n[0], move.n[1], x, y, delta.x, delta.y);

	switch (button) {
		case GLUT_LEFT_BUTTON: {
			c3mat4 rotx = rotation3D(c3->cam.side, delta.n[1] / 4);
			c3mat4 roty = rotation3D(c3vec3f(0.0, 0.0, 1.0), delta.n[0] / 4);
			rotx = c3mat4_mul(&rotx, &roty);
			c3cam_rot_about_lookat(&c3->cam, &rotx);

		    c3cam_update_matrix(&c3->cam);
		    c3->root->dirty = 1;	// resort the array
		}	break;
		case GLUT_RIGHT_BUTTON: {
			// offset both points, but following the plane
			c3vec3 f = c3vec3_mulf(c3vec3f(-c3->cam.side.y, c3->cam.side.x, 0), -delta.n[1] / 4);
			c3->cam.eye = c3vec3_add(c3->cam.eye, f);
			c3->cam.lookat = c3vec3_add(c3->cam.lookat, f);
			c3cam_movef(&c3->cam, delta.n[0] / 8, 0, 0);

		    c3cam_update_matrix(&c3->cam);
		    c3->root->dirty = 1;	// resort the array
		}	break;
	}
	move = m;
}

// gl timer. if the lcd is dirty, refresh display
static void
_gl_timer_cb(
		int i)
{
	glutTimerFunc(1000 / 24, _gl_timer_cb, 0);
	glutPostRedisplay();
}

int
gl_init(
		int argc,
		char *argv[] )
{
	glutInit(&argc, argv);		/* initialize GLUT system */

	glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH | GLUT_ALPHA);
	glutInitWindowSize(_w, _h);		/* width=400pixels height=500pixels */
	/*window =*/ glutCreateWindow("Press 'q' to quit");	/* create window */

	glutDisplayFunc(_gl_display_cb);		/* set window's display callback */
	glutKeyboardFunc(_gl_key_cb);		/* set window's key callback */
	glutTimerFunc(1000 / 24, _gl_timer_cb, 0);

	glutMouseFunc(_gl_button_cb);
	glutMotionFunc(_gl_motion_cb);
    glutReshapeFunc(_gl_reshape_cb);

	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);
	/*
	glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
	glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
	glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
	glEnable(GL_LINE_SMOOTH);
	 */
	// enable color tracking
	glEnable(GL_COLOR_MATERIAL);
	// set material properties which will be assigned by glColor
	glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);

	/* setup some lights */
	glShadeModel(GL_SMOOTH);
	glEnable(GL_LIGHTING);
	GLfloat global_ambient[] = { 0.5f, 0.5f, 0.5f, 1.0f };
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, global_ambient);

	{
		GLfloat specular[] = {1.0f, 1.0f, 1.0f , 0.8f};
		GLfloat position[] = { -50.0f, -50.0f, 100.0f, 1.0f };
		glLightfv(GL_LIGHT0, GL_SPECULAR, specular);
		glLightfv(GL_LIGHT0, GL_POSITION, position);
		glEnable(GL_LIGHT0);
	}
	{
		GLfloat specular[] = {1.0f, 1.0f, 1.0f , 0.8f};
		GLfloat position[] = { 250.0f, -50.0f, 100.0f, 1.0f };
		glLightfv(GL_LIGHT0, GL_SPECULAR, specular);
		glLightfv(GL_LIGHT0, GL_POSITION, position);
		glEnable(GL_LIGHT0);
	}

	/*
	 * Extract the GLSL version as a nuneric value for later
	 */
	const char * glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	{
		int M = 0, m = 0;
		if (sscanf(glsl, "%d.%d", &M, &m) == 2)
			glsl_version = (M * 100) + m;

	}
	printf("GL_SHADING_LANGUAGE_VERSION %s = %d\n", glsl, glsl_version);

	gl_offscreenInit(_w, _h);
	gl_ppProgram();

    c3 = c3context_new(_w, _h);
    static const c3driver_context_t * list[] = { &c3context_driver, NULL };
    c3->driver = list;

	c3->cam.lookat = c3vec3f(100.0, 100.0, 0.0);
	c3->cam.eye = c3vec3f(100.0, -100.0, 100.0);
    c3cam_update_matrix(&c3->cam);

    {
    	const char *path = "gfx/hb.png";
        cairo_surface_t * image = cairo_image_surface_create_from_png (path);
        printf("image = %p %p\n", image, cairo_image_surface_get_data (image));
    	c3texture_p b = c3texture_new(c3->root);

    	c3pixels_p dst = c3pixels_new(
    			cairo_image_surface_get_width (image),
    			cairo_image_surface_get_height (image),
    			4, cairo_image_surface_get_stride(image),
    			cairo_image_surface_get_data (image));
		dst->name = str_new(path);
    	b->geometry.mat.texture = dst;
    	b->size = c3vec2f(200, 200);
		b->geometry.mat.color = c3vec4f(1.0, 1.0, 1.0, 1.0);
//	    c3transform_new(head);
    }
    c3pixels_p line_aa_tex = NULL;
    {
    	const char *path = "gfx/BlurryCircle.png";
        cairo_surface_t * image = cairo_image_surface_create_from_png (path);
        printf("image = %p %p\n", image, cairo_image_surface_get_data (image));

#if 0
    	c3pixels_p dst = &b->pixels;
    	c3pixels_init(dst,
    			cairo_image_surface_get_width (image),
    			cairo_image_surface_get_height (image),
    			1, cairo_image_surface_get_width (image),
    			NULL);
    	c3pixels_alloc(dst);
    	b->size = c3vec2f(32, 32);
    	b->normalized = 1;

    	c3pixels_p src = c3pixels_new(
    			cairo_image_surface_get_width (image),
    			cairo_image_surface_get_height (image),
    			4, cairo_image_surface_get_stride(image),
    			cairo_image_surface_get_data (image));

    	uint32_t * _s = (uint32_t *)src->base;
    	uint8_t * _d = (uint8_t *)dst->base;
    	int max = 0;
    	for (int i = 0; i < dst->h * dst->w; i++)
    		if ((_s[i] & 0xff) > max)
    			max = _s[i] & 0xff;
    	for (int i = 0; i < dst->h * dst->w; i++)
    		*_d++ = ((_s[i] & 0xff) * 255) / max;// + (0xff - max);
    	b->pixels.format = C3PIXEL_A;
#else
    	c3pixels_p dst = c3pixels_new(
    			cairo_image_surface_get_width (image),
    			cairo_image_surface_get_height (image),
    			4, cairo_image_surface_get_stride(image),
    			cairo_image_surface_get_data (image));
    	dst->format = C3PIXEL_ARGB;
    	dst->normalize = 1;
    	dst->name = str_new(path);
    	uint8_t * line = dst->base;
    	for (int y = 0; y < dst->h; y++, line += dst->row) {
    		uint32_t *p = (uint32_t *)line;
    		for (int x = 0; x < dst->w; x++, p++) {
    			uint8_t b = *p;
    			*p = ((0xff - b) << 24);//|(b << 16)|(b << 8)|(b);
    		}
    	}
#endif
    	line_aa_tex = dst;

    	c3pixels_p p = dst;
    	printf("struct { int w, h, stride, size, format; uint8_t pix[] } img = {\n"
    			"%d, %d, %d, %d, %d\n",
    			p->w, p->h, (int)p->row, p->psize, cairo_image_surface_get_format(image));
    	for (int i = 0; i < 32; i++)
    		printf("0x%08x ", ((uint32_t*)p->base)[i]);
    	printf("\n");
    }
    c3object_p grid = c3object_new(c3->root);
    {
        for (int x = 0; x <= 20; x++) {
        	for (int y = 0; y <= 20; y++) {
        		c3vec3 p[4] = {
        			c3vec3f(-1+x*10,y*10,0.01), c3vec3f(1+x*10,y*10,0.01),
        			c3vec3f(x*10,-1+y*10,0.02), c3vec3f(x*10,1+y*10,0.02),
        		};
            	c3geometry_p g = c3geometry_new(
            			c3geometry_type(C3_LINES_TYPE, 0), grid);
            	g->mat.color = c3vec4f(0.0, 0.0, 0.0, 0.8);
            	g->mat.texture = line_aa_tex;
        		c3lines_init(g, p, 4, 0.2);
        	}
        }
    }

   if (0) {
		c3vec3 p[4] = {
			c3vec3f(-5,-5,1), c3vec3f(205,-5,1),
		};
    	c3geometry_p g = c3geometry_new(
    			c3geometry_type(C3_LINES_TYPE, 0), grid);
    	g->mat.color = c3vec4f(0.0, 0.0, 0.0, 1.0);
    	g->mat.texture = line_aa_tex;
    	g->line.width = 2;

		c3vertex_array_insert(&g->vertice,
				g->vertice.count, p, 2);

    }
    head = c3stl_load("gfx/buserror-nozzle-model.stl", c3->root);
    //head = c3object_new(c3->root);
    c3transform_new(head);
    if (head->geometry.count > 0) {
    	head->geometry.e[0]->mat.color = c3vec4f(0.6, 0.5, 0.0, 1.0);
    }

#if 0
    c3texture_p b = c3texture_new(head);
    c3pixels_init(&b->pixels, 64, 64, 4, 4 * 64, NULL);
    b->geometry.dirty = 1;
    memset(b->pixels.base, 0xff, 10 * b->pixels.row);
#endif


    hud = c3context_new(_w, _h);
    hud->driver = list;
    /*
     * This is the offscreen framebuffer where the 3D scene is drawn
     */
    if (FBO) {
    	c3texture_p b = c3texture_new(hud->root);

    	c3pixels_p dst = c3pixels_new(_w, _h, 4, _w * 4, NULL);
		dst->name = str_new("fbo");
		dst->texture = fbo_texture;
		dst->normalize = 1;
		dst->dirty = 0;
	//	dst->trace = 1;
    	b->geometry.mat.texture = dst;
    	b->geometry.mat.program.pid = program_postproc;
    	b->size = c3vec2f(_w, _h);
		b->geometry.mat.color = c3vec4f(1.0, 1.0, 1.0, 1.0);
		fbo_c3 = b;
    }

    {
		c3vec3 p[4] = {
			c3vec3f(10,10,0), c3vec3f(800-10,10,0),
		};
    	c3geometry_p g = c3geometry_new(
    			c3geometry_type(C3_LINES_TYPE, 0), hud->root);
    	g->mat.color = c3vec4f(0.5, 0.5, 1.0, .3f);
    	g->mat.texture = line_aa_tex;
		c3lines_init(g, p, 2, 10);
    }
	return 1;
}

void
gl_dispose()
{
	c3context_dispose(c3);
}

int
gl_runloop()
{
	glutMainLoop();
	gl_dispose();
	return 0;
}
