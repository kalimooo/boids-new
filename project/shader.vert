#version 430

layout(location = 0) in vec4 boid;

out vec2 boidPos;

void main()
{
	gl_Position = vec4(boid[0], boid[1], 0.0, 1.0);
	boidPos = vec2(boid[0], boid[1]);
}
