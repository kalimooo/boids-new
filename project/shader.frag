#version 430

// required by GLSL spec Sect 4.5.3 (though nvidia does not, amd does)
precision highp float;

uniform vec3 color;

layout(location = 0) out vec4 fragmentColor;

in vec2 boidPos;

void main()
{
	//fragmentColor = vec4(color, 1.0);
	fragmentColor = vec4((boidPos.x + 1) / 2, (boidPos.y + 1) / 2, 0.0f, 1.0f);
}
