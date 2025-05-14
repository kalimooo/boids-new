#version 420

precision highp float; 

layout(binding = 0) uniform sampler2D newFrameBufferTexture;
layout(binding = 1) uniform sampler2D oldFrameBufferTexture;
layout(location = 0) out vec4 fragmentColor;

uniform float blendFactor = 0.9;
uniform float decayFactor = 0.995;

/**
* Helper function to sample with pixel coordinates, e.g., (511.5, 12.75)
* This functionality is similar to using sampler2DRect.
* TexelFetch only work with integer coordinates and do not perform bilinerar filtering.
*/
vec4 textureRect(in sampler2D tex, vec2 rectangleCoord)
{
	return texture(tex, rectangleCoord / textureSize(tex, 0));
}

void main() {
    vec4 newTexFrag = textureRect(newFrameBufferTexture, gl_FragCoord.xy);
    vec4 oldTexFrag = textureRect(oldFrameBufferTexture, gl_FragCoord.xy) * decayFactor;
    
    //fragmentColor = newTexFrag;
    fragmentColor = newTexFrag + oldTexFrag;
    //fragmentColor = mix(newTexFrag, oldTexFrag, blendFactor);
}