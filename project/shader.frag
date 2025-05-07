#version 430

// required by GLSL spec Sect 4.5.3 (though nvidia does not, amd does)
precision highp float;

uniform float minSpeed;
uniform float maxSpeed;

layout(location = 0) out vec4 fragmentColor;

in vec2 boidSpeed;

void main()
{
	float speed = length(boidSpeed);
	//float t = smoothstep(minSpeed, maxSpeed, speed);
	float t = clamp((speed - minSpeed) / (maxSpeed - minSpeed), 0.0, 1.0);
	float redColor = mix(0.0, 1.0, t);
	//vec3 color = mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), t);
	vec3 color = vec3(redColor, 1 - redColor, 0.0);
	fragmentColor = vec4(color, 1.0);
}
