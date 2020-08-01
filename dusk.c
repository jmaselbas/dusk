#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/inotify.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

struct texture {
	GLenum unit;
	GLuint id;
};

#define LEN(a) (sizeof(a)/sizeof(*a))

int verbose;
char *argv0;
unsigned int width = 1080;
unsigned int height = 800;
GLFWwindow *window;
GLuint quad_vao;
GLuint quad_vbo;
GLuint sprg;
GLuint vshd;
GLuint fshd;

#define FFT_SIZE 512
struct texture tex_fft;

char *frag_name;
GLchar *frag;
GLuint frag_size;

char logbuf[4096];
GLsizei logsize;

#include <alsa/asoundlib.h>

snd_rawmidi_t *midi_rx;
unsigned char midi_cc[256];

#include <fftw3.h>
float fftw_in[FFT_SIZE], fftw_out[FFT_SIZE];
fftwf_plan plan;

#include <jack/jack.h>
#include <jack/midiport.h>

jack_client_t *jack;
jack_port_t *midi_port;
jack_port_t *input_port;

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	glfwTerminate();
	exit(1);
}

static GLuint
create_1dr32_tex(void *data, size_t size) {
	GLuint tex = 0;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_1D, tex);

	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	glTexImage1D(GL_TEXTURE_1D, 0, GL_R32F, size, 0, GL_RED, GL_FLOAT, data);

	glBindTexture(GL_TEXTURE_1D, 0);

	return tex;
}

static void
update_1dr32_tex(struct texture *tex, void *data, size_t size)
{
	glActiveTexture(tex->unit);
	glBindTexture(GL_TEXTURE_1D, tex->id);
	glTexSubImage1D(GL_TEXTURE_1D, 0, 0, size, GL_RED, GL_FLOAT, data);
}

static void
reloadshader(void)
{
	GLuint nprg = glCreateProgram();
	GLuint fshd = glCreateShader(GL_FRAGMENT_SHADER);
	FILE *frag_file = fopen(frag_name, "r");
	size_t size = 0;
	int ret;

	if (!frag_file) {
		fprintf(stderr, "%s: %s\n", frag_name, strerror(errno));
		return;
	}

	while (!feof(frag_file)) {
		if ((size + 1024) > frag_size)
			frag = realloc(frag, 1 + (frag_size += 1024));
		size += fread(frag + size, sizeof(char), 1024, frag_file);
	}
	frag[size] = '\0';
	fclose(frag_file);

	glShaderSource(fshd, 1, (const GLchar * const*)&frag, &frag_size);
	glCompileShader(fshd);
	glGetShaderiv(fshd, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		glGetShaderInfoLog(fshd, sizeof(logbuf), &logsize, logbuf);
		glDeleteProgram(nprg);
		glDeleteShader(fshd);
		printf("--- ERROR ---\n%s", logbuf);
		return;
	}

	glAttachShader(nprg, vshd);
	glAttachShader(nprg, fshd);
	glLinkProgram(nprg);
	glGetProgramiv(nprg, GL_LINK_STATUS, &ret);
	if (!ret) {
		glGetProgramInfoLog(nprg, sizeof(logbuf), &logsize, logbuf);
		glDeleteProgram(nprg);
		glDeleteShader(fshd);
		printf("--- ERROR ---\n%s", logbuf);
		return;
	}

	printf("--- LOADED --- (%d)\n", nprg);

	if (sprg)
		glDeleteProgram(sprg);

	glUseProgram(sprg = nprg);
}

static int
initshader(void)
{
	static float quad[] = {
		-1.0, -1.0,  0.5, 0.0, 0.0,
		-1.0,  1.0,  0.5, 0.0, 1.0,
		 1.0, -1.0,  0.5, 1.0, 0.0,
		 1.0,  1.0,  0.5, 1.0, 1.0,
	};
	const char *vert =
		"#version 410 core\n"
		"in vec3 in_pos;\n"
		"in vec2 in_texcoord;\n"
		"out vec2 out_texcoord;\n"
		"void main()\n"
		"{\n"
		"  gl_Position = vec4(in_pos.x, in_pos.y, in_pos.z, 1.0);\n"
		"  out_texcoord = in_texcoord;\n"
		"}\n";
	GLint size = strlen(vert);
	int ret;

	tex_fft.unit = GL_TEXTURE0;
	tex_fft.id = create_1dr32_tex(fftw_out, LEN(fftw_out));

	glGenVertexArrays(1, &quad_vao);
	glBindVertexArray(quad_vao);

	glGenBuffers(1, &quad_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindVertexArray(0);

	vshd = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vshd, 1, &vert, &size);
	glCompileShader(vshd);
	glGetShaderInfoLog(vshd, sizeof(logbuf), &logsize, logbuf);
	glGetShaderiv(vshd, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		glDeleteShader(fshd);
		printf("--- ERROR ---\n%s", logbuf);
		die("error in vertex shader\n");
	}

	reloadshader();
}

static void
render(void)
{
	GLint position;
	GLint texcoord;
	size_t stride;
	size_t offset;

	if (!sprg)
		return;

	glUseProgram(sprg);
	glBindVertexArray(quad_vao);

	stride = 5 * sizeof(float);
	offset = 3 * sizeof(float);

	position = glGetAttribLocation(sprg, "in_pos");
	if (position >= 0) {
		glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, stride, NULL);
		glEnableVertexArrayAttrib(quad_vao, position);
	}

	texcoord = glGetAttribLocation(sprg, "in_texcoord");
	if (texcoord >= 0) {
		glVertexAttribPointer(texcoord, 2, GL_FLOAT, GL_FALSE, stride, (GLvoid *)offset);
		glEnableVertexArrayAttrib(quad_vao, texcoord);
	}

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	if (texcoord >= 0)
		glDisableVertexArrayAttrib(quad_vao, texcoord);

	if (position >= 0)
		glDisableVertexArrayAttrib(quad_vao, position);

	glBindVertexArray(0);
}

static void
update(void)
{
	GLint location;
	char cc[] = "cc000";
	int i;

	location = glGetUniformLocation(sprg, "fGlobalTime");
	if (location >= 0)
		glProgramUniform1f(sprg, location, glfwGetTime());

	location = glGetUniformLocation(sprg, "v2Resolution");
	if (location >= 0)
		glProgramUniform2f(sprg, location, width, height);

	for (i = 0; i < 128; i++) {
		snprintf(cc, sizeof("cc000"), "cc%d", i);
		location = glGetUniformLocation(sprg, cc);
		if (location >= 0)
			glProgramUniform1f(sprg, location, midi_cc[i] / 127.0f);
	}

	location = glGetUniformLocation(sprg, "texFFT");
	if (location >= 0) {
		update_1dr32_tex(&tex_fft, fftw_out, LEN(fftw_out));

		glProgramUniform1i(sprg, location, tex_fft.unit);
		glActiveTexture(tex_fft.unit);
		glBindTexture(GL_TEXTURE_1D, tex_fft.id);
	}
}

static void
key_callback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
	const char *act;

	if (action == GLFW_PRESS) {
		switch (key) {
		case 'V':
			verbose = !verbose;
			printf("--- %s ---\n", verbose ? "verbose" : "quiet");

			break;
		case 'R':
			reloadshader();
			break;
		case 'C':
			if (mods & GLFW_MOD_CONTROL)
				glfwSetWindowShouldClose(window, GLFW_TRUE);
		default:
			break;
		}
	}
}

static
void framebuffer_callback(GLFWwindow* window, int w, int h)
{
	glViewport(0, 0, width = w, height = h);
}

static void
initglfw(void)
{
	if (!glfwInit())
		die("GLFW init failed\n");

	glfwWindowHint(GLFW_RED_BITS, 8);
	glfwWindowHint(GLFW_GREEN_BITS, 8);
	glfwWindowHint(GLFW_BLUE_BITS, 8);
	glfwWindowHint(GLFW_ALPHA_BITS, 8);
	glfwWindowHint(GLFW_DEPTH_BITS, 24);
	glfwWindowHint(GLFW_STENCIL_BITS, 8);

	glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);

	window = glfwCreateWindow(width, height, argv0, NULL, NULL);
	if (!window)
		die("create window failed\n");

	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);

	glfwGetFramebufferSize(window, &width, &height);
	glfwSetFramebufferSizeCallback(window, framebuffer_callback);

	glfwSetKeyCallback(window, key_callback);
}

static void
usage(void)
{
	printf("usage: %s <shader_file>\n", argv0);
	exit(1);
}

void
alsa_initmidi(void)
{
	char *dev;
	int ret;

	dev = getenv("MIDI_DEV");
	if (!dev)
		dev = "virtual";

	ret = snd_rawmidi_open(&midi_rx, NULL, dev, SND_RAWMIDI_NONBLOCK);
	if (ret < 0) {
		printf("snd_rawmidi_open: %d\n", ret);
		return;
	}

#if 0 /* TODO: create a virtual seq */
	// Set up the ALSA sequencer client.
	snd_seq_t *seq;
	int result = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
	if ( result < 0 ) {
		errorString_ = "MidiInAlsa::initialize: error creating ALSA sequencer client object.";
		error( RtMidiError::DRIVER_ERROR, errorString_ );
		return;
	}

	// Set client name.
	snd_seq_set_client_name( seq, clientName.c_str() );
#endif
}

void
midi_process(size_t size, unsigned char *buff)
{
	unsigned char sts, ccn, ccv;

	if (size < 3)
		return;

	sts = (buff[0] & 0xf0) >> 4;

	if (sts == 0xb) {
		/* control change */
		ccn = buff[1];
		ccv = buff[2];
		midi_cc[ccn] = ccv;
		if (verbose)
			printf("cc%d = %d\n", ccn, ccv);
	}
}

void
alsa_midi_update(void)
{
	static unsigned char midi_buf[1024];
	static size_t midi_len;
	unsigned char *buf;
	unsigned char sts, ccn, ccv;
	int rem;
	int ret;

	if (!midi_rx)
		return;

	ret = snd_rawmidi_read(midi_rx, midi_buf + midi_len, sizeof(midi_buf) - midi_len);
	if (ret < 0) {
		if (ret != -EAGAIN)
			printf("snd_rawmidi_read: %d %s\n", ret, snd_strerror(ret));
		return;
	}

	midi_len += ret;
	rem = midi_len;
	buf = midi_buf;
	for (; rem >= 3; rem--, buf++) {
		if (buf[0] < 0x80)
			continue; /* data byte */
		sts = (buf[0] & 0xf0) >> 4;

		midi_process(3, buf);
		rem -= 2;
		buf += 2;
	}
	memmove(midi_buf, buf, rem);
	midi_len = rem;
}

int
jack_process(jack_nframes_t frames, void* arg)
{
	void* buffer;
	jack_nframes_t n, i;
	jack_midi_event_t event;
	jack_default_audio_sample_t *in;
	int r;

	if (midi_port) {
		buffer = jack_port_get_buffer(midi_port, frames);

		n = jack_midi_get_event_count(buffer);
		for (i = 0; i < n; i++) {
			r = jack_midi_event_get (&event, buffer, i);
			if (r == 0)
				midi_process(event.size, event.buffer);
		}
	}

	if (input_port) {
		in = jack_port_get_buffer(input_port, frames);
		for (i = 0; i < frames; i++)
			fftw_in[i] = in[i];
		if (plan)
			fftwf_execute(plan);
	}

	return 0;
}

void
jack_init(void)
{
	int ret;

	jack = jack_client_open(argv0, JackNullOption, NULL);
	if (!jack) {
		printf("err creating jack client\n");
		return;
	}

	ret = jack_set_process_callback(jack, jack_process, 0);
	if (ret) {
		fprintf(stderr, "Could not register process callback.\n");
		return;

	}
	midi_port = jack_port_register(jack, "input", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	if (!midi_port) {
		fprintf(stderr, "Could not register midi port.\n");
		return;
	}

	input_port = jack_port_register(jack, "mono", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	if (!input_port) {
		fprintf(stderr, "Could not register audio port.\n");
		return;
	}

	ret = jack_activate(jack);
	if (ret) {
		fprintf(stderr, "Could not activate client.\n");
		return;
	}
}

void
jack_fini(void)
{
	jack_deactivate(jack);
	jack_client_close(jack);
}

#include <sys/inotify.h>
int notify;

void
init_inotify(void)
{
	int ret;

	notify = inotify_init1(IN_NONBLOCK);
	if (notify < 0) {
		printf("inotify fail %d\n", notify);
		return;
	}

	ret = inotify_add_watch(notify, frag_name, IN_MODIFY);
	if (ret < 0) {
		close(notify);
		notify = 0;
		printf("inotify add watch fail %d\n", ret);
		return;
	}
}

void
poll_inotify(void)
{
	struct inotify_event iev;
	ssize_t ret;

	do {
		ret = read(notify, &iev, sizeof(iev));
		if (ret > 0)
			reloadshader();
	} while (ret > 0);
}

int
main(int argc, char **argv)
{
	struct stat stat;
	int fd, ret;

	argv0 = argv[0];

	if (argc != 2)
		usage();

	fd = open(argv[1], O_RDONLY);
	if (fd == -1)
		die("%s: %s\n", argv[1], strerror(errno));
	ret = fstat(fd, &stat);
	if (ret == -1)
		die("stat: %s\n", argv[1], strerror(errno));
	if (!S_ISREG(stat.st_mode))
		die("%s: is not a regular file\n", argv[1]);
	close(fd);
	frag_name = argv[1];

	plan = fftwf_plan_r2r_1d(FFT_SIZE, fftw_in, fftw_out, FFTW_FORWARD, FFTW_ESTIMATE);

	jack_init();
	init_inotify();

	initglfw();

	ret = glewInit();
	if (ret != GLEW_OK)
		die("GLEW init failed: %s\n", glewGetErrorString(ret));

	initshader();
	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();
		poll_inotify();
		update();
		render();
		glfwSwapBuffers(window);
	}
	jack_fini();
	glfwTerminate();
	return 0;
}
