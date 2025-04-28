#version 430

// required by GLSL spec Sect 4.5.3 (though nvidia does not, amd does)
precision highp float;

uniform vec3 color;

layout(location = 0) out vec4 fragmentColor;

void main()
{
	//fragmentColor = vec4(color, 1.0);
	fragmentColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
}
