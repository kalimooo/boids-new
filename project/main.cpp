
#ifdef _WIN32
extern "C" _declspec(dllexport) unsigned int NvOptimusEnablement = 0x00000001;
#endif

#include <GL/glew.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>

#include <labhelper.h>
#include <imgui.h>

#include <perf.h>

#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>
using namespace glm;

#include <Model.h>
#include "hdr.h"
#include "fbo.h"

#include <stdio.h>


///////////////////////////////////////////////////////////////////////////////
// Various globals
///////////////////////////////////////////////////////////////////////////////
SDL_Window* g_window = nullptr;
float currentTime = 0.0f;
float previousTime = 0.0f;
float deltaTime = 0.0f;
int windowWidth, windowHeight;
bool isPaused = false;

// Mouse input
ivec2 g_prevMouseCoords = { -1, -1 };
bool g_isMouseDragging = false;
bool followMouse = false;
ivec2 mousePos = { -1, -1 };

///////////////////////////////////////////////////////////////////////////////
// Shader programs
///////////////////////////////////////////////////////////////////////////////
GLuint shaderProgram;       // Shader for rendering the final image

///////////////////////////////////////////////////////////////////////////////
// VAO
///////////////////////////////////////////////////////////////////////////////
GLuint vao;

///////////////////////////////////////////////////////////////////////////////
// Data for the particles
///////////////////////////////////////////////////////////////////////////////
struct particle {
	vec2 position;
	vec2 velocity;
	uint bucketIndex;
	uint gridIndex;
	float density;
	float padding;
	vec2 grad;
};

GLuint posVBO;
GLuint particleSSBO;


particle* particles;

///////////////////////////////////////////////////////////////////////////////
// Compute Shader stuff
///////////////////////////////////////////////////////////////////////////////
GLuint computeShaderProgram;

float visualRange = 0.25;
float protectedRange = 0.1;
float centeringFactor = 0.02;
float matchingFactor = 0.05;
float avoidFactor = 0.2;
float borderMargin = 0.1;
float turnFactor = 0.35;
float minSpeed = 0.2;
float maxSpeed = 0.3;
float randFactor = 0.05f;

///////////////////////////////////////////////////////////////////////////////
// Grid stuffs
///////////////////////////////////////////////////////////////////////////////
GLuint prefixSumSSBO;
GLuint* prefixSums = nullptr;
GLuint bucketSizesSSBO;
GLuint* bucketSizes = nullptr;

GLuint reorderedparticlesSSBO;

GLuint gridShaderProgram;
GLuint prefixSumShaderProgram;
GLuint reindexShaderProgram;
///////////////////////////////////////////////////////////////////////////////
// For blending
///////////////////////////////////////////////////////////////////////////////
FboInfo fbos[2];
GLuint blendProgram;
bool additiveBlending = true;

const int NUM_PARTICLES = 20;
const GLint gridSize = 2;

float kernelScalingFactor = 0.5f;
bool gravityEnabled = false;
float gravityStrength = 0.1f;
float smoothingRadius = 0.35f;
//float smoothingRadius = 2.0f / (float) gridSize;


void initGrid() {
	prefixSums = new GLuint[gridSize * gridSize];
	bucketSizes = new GLuint[gridSize * gridSize];
	for (int i = 0; i < gridSize * gridSize; i++) {
		prefixSums[i] = 0;
		bucketSizes[i] = 0;
	}

	glGenBuffers(1, &bucketSizesSSBO);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, bucketSizesSSBO);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * gridSize * gridSize, bucketSizes,
					GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, particleSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bucketSizesSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	// printf("Starting grid:\n");
	// for (int i = 0; i < gridSize * gridSize; i++) {
	// 	if (i != 0 && (i) % gridSize == 0) printf("\n");
	// 	printf("%d ", bucketSizes[i]);
	// }
	// printf("\n\n");
}

// TODO Maybe do this in a compute shader as well
void calculatePrefixSum() {
    prefixSums[0] = 0;
    for (int i = 1; i < gridSize * gridSize; ++i) {
        prefixSums[i] = prefixSums[i - 1] + bucketSizes[i - 1];
    }

    // Update the GPU buffer with the new prefix sums
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, prefixSumSSBO);

    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint) * gridSize * gridSize, prefixSums);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	// printf("Prefix sums:\n");
	// for (int i = 0; i < gridSize * gridSize; i++) {
	// 	if (i != 0 && (i) % gridSize == 0) printf("\n");
	// 	printf("%d ", prefixSums[i]);
	// }
	// printf("\n\n");
}

void reindexparticles() {
	glUseProgram(reindexShaderProgram);

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, particleSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, prefixSumSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bucketSizesSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, reorderedparticlesSSBO);

	glDispatchCompute(NUM_PARTICLES, 1, 1);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
	
	glCopyNamedBufferSubData(reorderedparticlesSSBO, particleSSBO, 0, 0, sizeof(particle) * NUM_PARTICLES);
}

void updateGrid() {
	labhelper::perf::Scope s( "Update Grid" );
	{
		labhelper::perf::Scope s( "Calculate bucket sizes" );
		// Reset bucketSizes buffer on the GPU
		memset(bucketSizes, 0, sizeof(GLuint) * gridSize * gridSize);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, bucketSizesSSBO);
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint) * gridSize * gridSize, bucketSizes);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		
		// Dispatch compute shader to calculate bucket sizes
		glUseProgram(gridShaderProgram);
		labhelper::setUniformSlow(gridShaderProgram, "gridSize", gridSize);
		// labhelper::setUniformSlow(gridShaderProgram, "gridCellSize", 1.0f / gridSize);
		glDispatchCompute(NUM_PARTICLES, 1, 1);
		glMemoryBarrier(GL_ALL_BARRIER_BITS);

		// Map bucketSizes buffer back to CPU
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, bucketSizesSSBO);
		bucketSizes = (GLuint*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(uint) * gridSize * gridSize, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
		if (bucketSizes == nullptr) {
			printf("Error: Failed to map bucketSizes buffer.\n");
			return;
		}

		// Unmap the buffer
		if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)) {
			printf("Error: Failed to unmap bucketSizes buffer.\n");
			return;
		}
	}

    // Calculate prefix sum on the CPU
	{
		labhelper::perf::Scope s( "Calculate prefix sum" );
		calculatePrefixSum();
	}

	{
		labhelper::perf::Scope s( "Reindex particles" );
		reindexparticles();
	}

	// for (int i = 0; i < NUM_PARTICLES; i++) {
	// 	printf("particle %d: (%.2f, %.2f) -> %d, %d\n", i, particles[i].position.x, particles[i].position.y, particles[i].gridIndex, particles[i].bucketIndex);
	// }
	// printf("BucketSizes:\n");
	// for (int i = 0; i < gridSize * gridSize; i++) {
	// 	if (i != 0 && (i) % gridSize == 0) printf("\n");
	// 	printf("%d ", bucketSizes[i]);
	// }
	// printf("\n\n");
}

void initializeparticles()
{
	float margin = 0.1f; // Margin to avoid particles being too close to the edges
	float range = 1.0f - margin;

	particles = new particle[NUM_PARTICLES];

	for (int i = 0; i < NUM_PARTICLES; ++i)
	{
		// Generate random position within the range [-1 + margin, 1 - margin]
        float x = margin + static_cast<float>(rand()) / RAND_MAX * (2.0f * range) - range;
        float y = margin + static_cast<float>(rand()) / RAND_MAX * (2.0f * range) - range;

        // Add slight random perturbation
        float perturbationX = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 0.05f;
        float perturbationY = (static_cast<float>(rand()) / RAND_MAX - 0.5f) * 0.05f;

        particles[i].position = vec2(x + perturbationX, y + perturbationY);

        // Initialize velocity with a random direction and magnitude
        //float angle = static_cast<float>(rand()) / RAND_MAX * 2.0f * M_PI;
        //float speed = minSpeed + static_cast<float>(rand()) / RAND_MAX * (maxSpeed - minSpeed);
        //particles[i].velocity = vec2(cos(angle), sin(angle)) * speed;
		particles[i].velocity = vec2(0.0f); // Start with zero velocity

		// Initialize bucket index and grid index
		particles[i].bucketIndex = 0;
		particles[i].gridIndex = 0;

		// Initialize density to zero
		particles[i].density = 0.0f;
	}

    // // Calculate the angular spacing between particles
    // float angleStep = 2.0f * M_PI / NUM_PARTICLES;
	// particles = new particle[NUM_PARTICLES];

    // for (int i = 0; i < NUM_PARTICLES; ++i)
    // {
    //     // Calculate the angle for this particle
    //     float angle = i * angleStep;

	// 	// Everything spawns in the middle
	// 	particles[i].position = vec2(0.0f);

    //     // Set the velocity to point away from the center (is already normalized due to being on identity circle)
    //     particles[i].velocity = vec2(cos(angle), sin(angle));
		
	// 	particles[i].position += vec2((float) i * 0.5 / (float) NUM_PARTICLES) * particles[i].velocity;
    // }
}

void updateparticleVertices()
{
	glUseProgram(shaderProgram);
	glBindBuffer(GL_ARRAY_BUFFER, posVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(particle) * NUM_PARTICLES, particles); // Update the VBO with current particle data
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void updateparticlePositions(float deltaTime, bool use_GPU)
{
	{	
		labhelper::perf::Scope s( "Update particles" );
		if (followMouse)
		{
			// Convert mouse position to normalized device coordinates (NDC)
			vec2 mouseNDC = vec2(
				((float) mousePos.x / (float) windowWidth - 0.5f) * 2.0f,
				1.0f - (2.0f * (float)mousePos.y) / (float)windowHeight);

			printf("%.2f, %.2f \n", (float) mouseNDC.x, (float)mouseNDC.y);
			for (int i = 0; i < NUM_PARTICLES; i++)
			{
				// Move particles toward the mouse position
				vec2 direction = normalize(mouseNDC - particles[i].position);
				particles[i].velocity = direction * maxSpeed;
				particles[i].position += particles[i].velocity * deltaTime;
			}
			updateparticleVertices();
			return;
		}

		if (use_GPU) {
			glUseProgram(computeShaderProgram);

			labhelper::setUniformSlow(computeShaderProgram, "deltaTime", deltaTime);
			labhelper::setUniformSlow(computeShaderProgram, "time", currentTime);
			labhelper::setUniformSlow(computeShaderProgram, "gridSize", gridSize);

			float mouseX = (2.0f * mousePos.x) / windowWidth - 1.0f;
			float mouseY = 1.0f - (2.0f * mousePos.y) / windowHeight;

			labhelper::setUniformSlow(computeShaderProgram, "mouseX", mouseX);
			labhelper::setUniformSlow(computeShaderProgram, "mouseY", mouseY);
			labhelper::setUniformSlow(computeShaderProgram, "kernelScalingFactor", kernelScalingFactor);
			labhelper::setUniformSlow(computeShaderProgram, "smoothingRadius", smoothingRadius);
			labhelper::setUniformSlow(computeShaderProgram, "gravityEnabled", gravityEnabled);
			labhelper::setUniformSlow(computeShaderProgram, "gravityStrength", gravityStrength);

			// labhelper::setUniformSlow(computeShaderProgram, "visualRange", visualRange);
			// labhelper::setUniformSlow(computeShaderProgram, "protectedRange", protectedRange);
			// labhelper::setUniformSlow(computeShaderProgram, "centeringFactor", centeringFactor);
			// labhelper::setUniformSlow(computeShaderProgram, "matchingFactor", matchingFactor);
			// labhelper::setUniformSlow(computeShaderProgram, "avoidFactor", avoidFactor);
			// labhelper::setUniformSlow(computeShaderProgram, "borderMargin", borderMargin);
			// labhelper::setUniformSlow(computeShaderProgram, "turnFactor", turnFactor);
			// labhelper::setUniformSlow(computeShaderProgram, "minSpeed", minSpeed);
			// labhelper::setUniformSlow(computeShaderProgram, "maxSpeed", maxSpeed);
			// labhelper::setUniformSlow(computeShaderProgram, "randFactor", randFactor);

			glBindBuffer(GL_SHADER_STORAGE_BUFFER, particleSSBO);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, particleSSBO);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, prefixSumSSBO);

			GLint bufMask = GL_MAP_WRITE_BIT;

			// printf("Positions before shader: ");
			// for (int i = 0; i < NUM_PARTICLES; i++) {
			// 	printf("(%.2f, %.2f), ", particles[i].position.x, particles[i].position.y);
			// }
			// printf("\n");
			
			glDispatchCompute(NUM_PARTICLES, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

			glBindBuffer(GL_SHADER_STORAGE_BUFFER, particleSSBO);
			particles = (particle*) glMapBufferRange( GL_SHADER_STORAGE_BUFFER, 0, sizeof(particle) * NUM_PARTICLES, bufMask);
			if (particles == nullptr) {
				printf("Error: Failed to map buffer.\n");
				return;
			}
			// printf("Positions after shader: ");
			// for (int i = 0; i < NUM_PARTICLES; i++) {
			// 	printf("(%.2f, %.2f), ", particles[i].position.x, particles[i].position.y);			
			// }
			// printf("\n\n");

			printf("0: Pos: (%.3f, %.3f), Density: %.3f, Gradient: (%.3f, %.3f)\n", particles[0].position.x, particles[0].position.y, particles[0].density, particles[0].grad.x, particles[0].grad.y);
			printf("1: Pos: (%.3f, %.3f), Density: %.3f, Gradient: (%.3f, %.3f)\n", particles[1].position.x, particles[1].position.y, particles[1].density, particles[1].grad.x, particles[1].grad.y);
			//printf("%.2f\n", mouseX);
			
			if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)) {
				printf("Error: Failed to unmap buffer.\n");
				return;
			}
		}
		else {
			for (int i = 0; i < NUM_PARTICLES; i++) {
				particles[i].position += vec2(0.01f) * deltaTime;
			}
		}

		updateparticleVertices();
	}
}

void loadShaders(bool is_reload)
{
	GLuint shader = labhelper::loadShaderProgram("../project/shader.vert", "../project/shader.frag", is_reload);
	if(shader != 0)
	{
		shaderProgram = shader;
	}

	// shader = labhelper::loadComputeShaderProgram("../project/particle.comp", is_reload);
	// if(shader != 0)
	// {
	// 	computeShaderProgram = shader;
	// }
	
	shader = labhelper::loadComputeShaderProgram("../project/particle.comp", is_reload);
	if(shader != 0)
	{
		computeShaderProgram = shader;
	}

	shader = labhelper::loadShaderProgram("../project/blend.vert", "../project/blend.frag", is_reload);
	if(shader != 0)
	{
		blendProgram = shader;
	}

	shader = labhelper::loadComputeShaderProgram("../project/grid.comp", is_reload);
	if(shader != 0)
	{
		gridShaderProgram = shader;
	}

	shader = labhelper::loadComputeShaderProgram("../project/reindex.comp", is_reload);
	if (shader != 0) {
		reindexShaderProgram = shader;
	}

	// shader = labhelper::loadComputeShaderProgram("../project/prefixSum.comp", is_reload);
	// if(shader != 0)
	// {
	// 	prefixSumShaderProgram = shader;
	// }
}

///////////////////////////////////////////////////////////////////////////////
/// This function is called once at the start of the program and never again
///////////////////////////////////////////////////////////////////////////////
void initialize()
{
	ENSURE_INITIALIZE_ONLY_ONCE();

	///////////////////////////////////////////////////////////////////////
	//		Load Shaders
	///////////////////////////////////////////////////////////////////////
	loadShaders(false);

	
	initializeparticles();

	///////////////////////////////////////////////////////////////////////
	// Generate and bind buffers for graphics pipeline
	///////////////////////////////////////////////////////////////////////
	glGenBuffers(1, &posVBO);
	glBindBuffer(GL_ARRAY_BUFFER, posVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(particle) * NUM_PARTICLES, particles, GL_DYNAMIC_DRAW);

	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(particle), 0);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

	///////////////////////////////////////////////////////////////////////
	// Generate and bind buffers for compute shaders
	///////////////////////////////////////////////////////////////////////
	// Positions
	glGenBuffers(1, &particleSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, particleSSBO);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(particle) * NUM_PARTICLES, particles,
					GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, particleSSBO);

	///////////////////////////////////////////////////////////////////////
	// Generate and bind buffers for compute shaders
	///////////////////////////////////////////////////////////////////////
	// Grid
	glGenBuffers(1, &prefixSumSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, prefixSumSSBO);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * gridSize * gridSize, prefixSums,
					GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, prefixSumSSBO);

	// Reindexed particles
	glGenBuffers(1, &reorderedparticlesSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, reorderedparticlesSSBO);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(particle) * NUM_PARTICLES, nullptr,
					GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, reorderedparticlesSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	// glGenBuffers(1, &bucketSizesSSBO);
	// glBindBuffer(GL_SHADER_STORAGE_BUFFER, bucketSizesSSBO);
	// glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(uint) * gridSize * gridSize, bucketSizes,
	// 				GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT);
	// glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, particleSSBO);
	// glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bucketSizesSSBO);
	// glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	initGrid();

	int w, h;
	SDL_GetWindowSize(g_window, &w, &h);
	for (int i = 0; i < 2; i++) {
		fbos[i] = FboInfo();
		fbos[i].resize(w, h);
	}
	
	glEnable(GL_DEPTH_TEST); // enable Z-buffering
	glPointSize(5.0f);

	labhelper::hideGUI();
	
	//glEnable(GL_CULL_FACE);  // enables backface culling
}


///////////////////////////////////////////////////////////////////////////////
/// This function will be called once per frame, so the code to set up
/// the scene for rendering should go here
///////////////////////////////////////////////////////////////////////////////
void display(void)
{
	labhelper::perf::Scope s( "Display" );

	///////////////////////////////////////////////////////////////////////////
	// Check if window size has changed and resize buffers as needed
	///////////////////////////////////////////////////////////////////////////
	{
		int w, h;
		SDL_GetWindowSize(g_window, &w, &h);
		if(w != windowWidth || h != windowHeight)
		{
			windowWidth = w;
			windowHeight = h;
		}

		for(int i = 0; i < 2; i++)
		{
			if(fbos[i].width != w || fbos[i].height != h)
				fbos[i].resize(w, h);
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Draw from camera
	///////////////////////////////////////////////////////////////////////////
	FboInfo &currentFB = fbos[0];
	FboInfo &oldFB = fbos[1];
	
	glBindFramebuffer(GL_FRAMEBUFFER, currentFB.framebufferId);
	glViewport(0, 0, currentFB.width, currentFB.height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// {
	// 	labhelper::perf::Scope s( "Background" );
	// 	drawBackground(viewMatrix, projMatrix);
	// }

	// Render scene
	{
		labhelper::perf::Scope s( "Scene" );
		
		glUseProgram(shaderProgram);
		labhelper::setUniformSlow(shaderProgram, "minSpeed", minSpeed);
		labhelper::setUniformSlow(shaderProgram, "maxSpeed", maxSpeed);
		glBindVertexArray(vao);
		glDrawArrays(GL_POINTS, 0, NUM_PARTICLES);
		glBindVertexArray(0);
	}
	{
		labhelper::perf::Scope s( "Blending" );
		// Blend new scene with accumulated trail
		glBindFramebuffer(GL_FRAMEBUFFER, oldFB.framebufferId);
		glViewport(0, 0, oldFB.width, oldFB.height);

		glUseProgram(blendProgram);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, currentFB.colorTextureTargets[0]);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, oldFB.colorTextureTargets[0]);

		if (additiveBlending) {
			labhelper::setUniformSlow(blendProgram, "blendFactor", 0.95f);
			labhelper::setUniformSlow(blendProgram, "decayFactor", 0.8f);
		} else {
			labhelper::setUniformSlow(blendProgram, "blendFactor", 0.85f);
			labhelper::setUniformSlow(blendProgram, "decayFactor", 1.0f);	
		}
		labhelper::setUniformSlow(blendProgram, "additiveBlending", additiveBlending);
		
		labhelper::drawFullScreenQuad();

		// Render the blended scene to default
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, windowWidth, windowHeight);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUseProgram(blendProgram);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, oldFB.colorTextureTargets[0]);

		labhelper::setUniformSlow(blendProgram, "blendFactor", 0.0f);
		labhelper::setUniformSlow(blendProgram, "decayFactor", 1.0f);
		labhelper::drawFullScreenQuad();
	}
}


///////////////////////////////////////////////////////////////////////////////
/// This function is used to update the scene according to user input
///////////////////////////////////////////////////////////////////////////////
bool handleEvents(void)
{
	// check events (keyboard among other)
	SDL_Event event;
	bool quitEvent = false;
	while(SDL_PollEvent(&event))
	{
		labhelper::processEvent( &event );

		if(event.type == SDL_QUIT || (event.type == SDL_KEYUP && event.key.keysym.sym == SDLK_ESCAPE))
		{
			quitEvent = true;
		}
		if(event.type == SDL_KEYUP && event.key.keysym.sym == SDLK_SPACE)
		{
			isPaused = !isPaused;
		}
		if(event.type == SDL_KEYUP && event.key.keysym.sym == SDLK_g)
		{
			if ( labhelper::isGUIvisible() )
			{
				labhelper::hideGUI();
			}
			else
			{
				labhelper::showGUI();
			}
		 
		}
		
		if (event.type == SDL_MOUSEMOTION)
		{
			mousePos.x = event.motion.x;
			mousePos.y = event.motion.y;
		}
		
	}
	
	return quitEvent;
}


///////////////////////////////////////////////////////////////////////////////
/// This function is to hold the general GUI logic
///////////////////////////////////////////////////////////////////////////////
void gui()
{
	// ----------------- Set variables --------------------------
	ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
	            ImGui::GetIO().Framerate);
	// ----------------------------------------------------------

	ImGui::Text("Blending parameters:");
	ImGui::Checkbox("Additive blending", &additiveBlending);

	ImGui::Text("Mouse control:");
	ImGui::Checkbox("Follow mouse", &followMouse);

	ImGui::Text("particle parameters:");
	ImGui::SliderFloat("kernelScalingFactor", &kernelScalingFactor, 0.01f, 10.0f);
	ImGui::SliderFloat("smoothingRadius", &smoothingRadius, 0.01f, 2.0f / (float)gridSize);
	ImGui::Checkbox("Gravity enabled", &gravityEnabled);
	ImGui::SliderFloat("gravityStrength", &gravityStrength, 0.0f, 1.0f);

	// ImGui::SliderFloat("visualRange", &visualRange, 0.0f, 2.0f);
	// ImGui::SliderFloat("protectedRange", &protectedRange, 0.0f, 1.0f);
	// ImGui::SliderFloat("centeringFactor", &centeringFactor, 0.0f, 0.1f);
	// ImGui::SliderFloat("matchingFactor", &matchingFactor, 0.0f, 0.1f);
	// ImGui::SliderFloat("avoidFactor", &avoidFactor, 0.0f, 0.5f);
	// ImGui::SliderFloat("borderMargin", &matchingFactor, 0.0f, 0.3f);
	// ImGui::SliderFloat("turnFactor", &turnFactor, 0.0f, 0.5f);
	// ImGui::SliderFloat("minSpeed", &minSpeed, 0.0f, 0.5f);
	// ImGui::SliderFloat("maxSpeed", &maxSpeed, 0.0f, 1.0f);
	// ImGui::SliderFloat("randFactor", &randFactor, 0.0f, 1.0f);



	////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////

	labhelper::perf::drawEventsWindow();
}

int main(int argc, char* argv[])
{
	g_window = labhelper::init_window_SDL("OpenGL Project");

	initialize();

	bool stopRendering = false;
	auto startTime = std::chrono::system_clock::now();

	while(!stopRendering)
	{
		//update currentTime
		std::chrono::duration<float> timeSinceStart = std::chrono::system_clock::now() - startTime;
		previousTime = currentTime;
		currentTime = timeSinceStart.count();
		deltaTime = currentTime - previousTime;

		// check events (keyboard among other)
		stopRendering = handleEvents();

		if (isPaused)
		{
			continue;
		}

		// Inform imgui of new frame
		labhelper::newFrame( g_window );
		
		// Update particles
		updateparticlePositions(deltaTime, true);

		updateGrid();
		
		// render to window
		display();

		// Render overlay GUI.
		gui();

		// Finish the frame and render the GUI
		labhelper::finishFrame();

		// Swap front and back buffer. This frame will now been displayed.
		SDL_GL_SwapWindow(g_window);
	}

	// Shut down everything. This includes the window and all other subsystems.
	labhelper::shutDown(g_window);
	return 0;
}
