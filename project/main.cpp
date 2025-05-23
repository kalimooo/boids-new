
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
// Data for the boids
///////////////////////////////////////////////////////////////////////////////
struct boid {
	vec2 position;
	vec2 velocity;
	uint bucketIndex;
	uint gridIndex;
};

GLuint posVBO;
GLuint boidSSBO;

const int NUM_BOIDS = 100;

boid* boids;

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
const GLint gridSize = 4;
GLuint prefixSumSSBO;
GLuint* prefixSums = nullptr;
GLuint bucketSizesSSBO;
GLuint* bucketSizes = nullptr;

GLuint reorderedBoidsSSBO;

GLuint gridShaderProgram;
GLuint prefixSumShaderProgram;
GLuint reindexShaderProgram;
///////////////////////////////////////////////////////////////////////////////
// For blending
///////////////////////////////////////////////////////////////////////////////
FboInfo fbos[2];
GLuint blendProgram;
bool additiveBlending = false;

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
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, boidSSBO);
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

void reindexBoids() {
	glUseProgram(reindexShaderProgram);

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, boidSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, prefixSumSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bucketSizesSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, reorderedBoidsSSBO);

	glDispatchCompute(NUM_BOIDS, 1, 1);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
	
	glCopyNamedBufferSubData(reorderedBoidsSSBO, boidSSBO, 0, 0, sizeof(boid) * NUM_BOIDS);
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

		// for (int i = 0; i < gridSize * gridSize; i++) {
		// 	if (i != 0 && (i) % gridSize == 0) printf("\n");
		// 	printf("%d ", bucketSizes[i]);
		// }
		// printf("\n\n");
		
		// Dispatch compute shader to calculate bucket sizes
		glUseProgram(gridShaderProgram);
		labhelper::setUniformSlow(gridShaderProgram, "gridSize", gridSize);
		// labhelper::setUniformSlow(gridShaderProgram, "gridCellSize", 1.0f / gridSize);
		glDispatchCompute(NUM_BOIDS, 1, 1);
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
		labhelper::perf::Scope s( "Reindex boids" );
		reindexBoids();
	}

	// for (int i = 0; i < NUM_BOIDS; i++) {
	// 	printf("Boid %d: (%.2f, %.2f) -> %d, %d\n", i, boids[i].position.x, boids[i].position.y, boids[i].gridIndex, boids[i].bucketIndex);
	// }
	// printf("BucketSizes:\n");
	// for (int i = 0; i < gridSize * gridSize; i++) {
	// 	if (i != 0 && (i) % gridSize == 0) printf("\n");
	// 	printf("%d ", bucketSizes[i]);
	// }
	// printf("\n\n");
}

void initializeBoids()
{
    // Calculate the angular spacing between boids
    float angleStep = 2.0f * M_PI / NUM_BOIDS;
	boids = new boid[NUM_BOIDS];

    for (int i = 0; i < NUM_BOIDS; ++i)
    {
        // Calculate the angle for this boid
        float angle = i * angleStep;

		// Everything spawns in the middle
		boids[i].position = vec2(0.0f);

        // Set the velocity to point away from the center (is already normalized due to being on identity circle)
        boids[i].velocity = vec2(cos(angle), sin(angle));
		
		boids[i].position += vec2(0.1f) * boids[i].velocity;
    }
}

void updateBoidVertices()
{
	glUseProgram(shaderProgram);
	glBindBuffer(GL_ARRAY_BUFFER, posVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(boid) * NUM_BOIDS, boids); // Update the VBO with current boid data
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void updateBoidPositions(float deltaTime, bool use_GPU)
{
	{	
		labhelper::perf::Scope s( "Update Boids" );
		if (followMouse)
		{
			// Convert mouse position to normalized device coordinates (NDC)
			vec2 mouseNDC = vec2(
				(2.0f * mousePos.x) / windowWidth - 1.0f,
				1.0f - (2.0f * mousePos.y) / windowHeight);

			printf("%d, %d \n", mousePos.x, mousePos.y);
			for (int i = 0; i < NUM_BOIDS; i++)
			{
				// Move boids toward the mouse position
				vec2 direction = normalize(mouseNDC - boids[i].position);
				boids[i].velocity = direction * maxSpeed;
				boids[i].position += boids[i].velocity * deltaTime;
			}
			updateBoidVertices();
			return;
		}

		if (use_GPU) {
			glUseProgram(computeShaderProgram);

			labhelper::setUniformSlow(computeShaderProgram, "deltaTime", deltaTime);
			labhelper::setUniformSlow(computeShaderProgram, "visualRange", visualRange);
			labhelper::setUniformSlow(computeShaderProgram, "protectedRange", protectedRange);
			labhelper::setUniformSlow(computeShaderProgram, "centeringFactor", centeringFactor);
			labhelper::setUniformSlow(computeShaderProgram, "matchingFactor", matchingFactor);
			labhelper::setUniformSlow(computeShaderProgram, "avoidFactor", avoidFactor);
			labhelper::setUniformSlow(computeShaderProgram, "borderMargin", borderMargin);
			labhelper::setUniformSlow(computeShaderProgram, "turnFactor", turnFactor);
			labhelper::setUniformSlow(computeShaderProgram, "minSpeed", minSpeed);
			labhelper::setUniformSlow(computeShaderProgram, "maxSpeed", maxSpeed);
			labhelper::setUniformSlow(computeShaderProgram, "time", currentTime);
			labhelper::setUniformSlow(computeShaderProgram, "randFactor", randFactor);
			labhelper::setUniformSlow(computeShaderProgram, "gridSize", gridSize);

			glBindBuffer(GL_SHADER_STORAGE_BUFFER, boidSSBO);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, boidSSBO);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, prefixSumSSBO);

			GLint bufMask = GL_MAP_WRITE_BIT;

			// printf("Positions before shader: ");
			// for (int i = 0; i < NUM_BOIDS; i++) {
			// 	printf("(%.2f, %.2f), ", boids[i].position.x, boids[i].position.y);
			// }
			// printf("\n");
			
			glDispatchCompute(NUM_BOIDS, 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

			glBindBuffer(GL_SHADER_STORAGE_BUFFER, boidSSBO);
			boids = (boid*) glMapBufferRange( GL_SHADER_STORAGE_BUFFER, 0, sizeof(boid) * NUM_BOIDS, bufMask);
			if (boids == nullptr) {
				printf("Error: Failed to map buffer.\n");
				return;
			}
			// printf("Positions after shader: ");
			// for (int i = 0; i < NUM_BOIDS; i++) {
			// 	printf("(%.2f, %.2f), ", boids[i].position.x, boids[i].position.y);			
			// }
			// printf("\n\n");
			
			if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)) {
				printf("Error: Failed to unmap buffer.\n");
				return;
			}
		}
		else {
			for (int i = 0; i < NUM_BOIDS; i++) {
				boids[i].position += vec2(0.01f) * deltaTime;
			}
		}

		updateBoidVertices();
	}
}

void loadShaders(bool is_reload)
{
	GLuint shader = labhelper::loadShaderProgram("../project/shader.vert", "../project/shader.frag", is_reload);
	if(shader != 0)
	{
		shaderProgram = shader;
	}

	shader = labhelper::loadComputeShaderProgram("../project/shader.compute", is_reload);
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

	
	initializeBoids();

	///////////////////////////////////////////////////////////////////////
	// Generate and bind buffers for graphics pipeline
	///////////////////////////////////////////////////////////////////////
	glGenBuffers(1, &posVBO);
	glBindBuffer(GL_ARRAY_BUFFER, posVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(boid) * NUM_BOIDS, boids, GL_DYNAMIC_DRAW);

	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(boid), 0);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

	///////////////////////////////////////////////////////////////////////
	// Generate and bind buffers for compute shaders
	///////////////////////////////////////////////////////////////////////
	// Positions
	glGenBuffers(1, &boidSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, boidSSBO);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(boid) * NUM_BOIDS, boids,
					GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, boidSSBO);

	///////////////////////////////////////////////////////////////////////
	// Generate and bind buffers for compute shaders
	///////////////////////////////////////////////////////////////////////
	// Grid
	glGenBuffers(1, &prefixSumSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, prefixSumSSBO);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * gridSize * gridSize, prefixSums,
					GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, prefixSumSSBO);

	// Reindexed boids
	glGenBuffers(1, &reorderedBoidsSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, reorderedBoidsSSBO);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(boid) * NUM_BOIDS, nullptr,
					GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_DYNAMIC_STORAGE_BIT);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, reorderedBoidsSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	// glGenBuffers(1, &bucketSizesSSBO);
	// glBindBuffer(GL_SHADER_STORAGE_BUFFER, bucketSizesSSBO);
	// glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(uint) * gridSize * gridSize, bucketSizes,
	// 				GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_DYNAMIC_STORAGE_BIT);
	// glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, boidSSBO);
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
		glDrawArrays(GL_POINTS, 0, NUM_BOIDS);
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

	ImGui::Text("Boid parameters:");
	ImGui::SliderFloat("visualRange", &visualRange, 0.0f, 2.0f);
	ImGui::SliderFloat("protectedRange", &protectedRange, 0.0f, 1.0f);
	ImGui::SliderFloat("centeringFactor", &centeringFactor, 0.0f, 0.1f);
	ImGui::SliderFloat("matchingFactor", &matchingFactor, 0.0f, 0.1f);
	ImGui::SliderFloat("avoidFactor", &avoidFactor, 0.0f, 0.5f);
	ImGui::SliderFloat("borderMargin", &matchingFactor, 0.0f, 0.3f);
	ImGui::SliderFloat("turnFactor", &turnFactor, 0.0f, 0.5f);
	ImGui::SliderFloat("minSpeed", &minSpeed, 0.0f, 0.5f);
	ImGui::SliderFloat("maxSpeed", &maxSpeed, 0.0f, 1.0f);
	ImGui::SliderFloat("randFactor", &randFactor, 0.0f, 1.0f);



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

		// Inform imgui of new frame
		labhelper::newFrame( g_window );
		
		// Update boids
		updateBoidPositions(deltaTime, true);

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
