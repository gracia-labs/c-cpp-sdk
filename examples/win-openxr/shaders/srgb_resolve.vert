#version 450

// A fullscreen triangle from the vertex index alone: three vertices, no vertex
// buffer, no input layout. Draw it with vkCmdDraw(cmd, 3, 1, 0, 0).
void main() {
  vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
