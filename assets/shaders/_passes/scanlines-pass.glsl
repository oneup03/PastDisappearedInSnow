// Dark lines between the SNES's pixel rows, the classic RetroArch look. Pair it with
// integer scaling so every line comes out the same thickness.

#ifdef VERTEX
in vec2 VertexCoord;
in vec2 TexCoord;
out vec2 vTexCoord;

void main() {
    vTexCoord = TexCoord;
    gl_Position = vec4(VertexCoord * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
uniform sampler2D Texture;
uniform vec2 InputSize;
in vec2 vTexCoord;
out vec4 FragColor;

const float LINE_STRENGTH = 0.45;
const float LINE_WIDTH = 0.28;

void main() {
    vec4 c = texture(Texture, vTexCoord);
    float row = fract(vTexCoord.y * InputSize.y);
    float distance_from_gap = min(row, 1.0 - row);
    float gap = 1.0 - smoothstep(LINE_WIDTH * 0.5, LINE_WIDTH, distance_from_gap);
    FragColor = vec4(c.rgb * (1.0 - LINE_STRENGTH * gap), c.a);
}
#endif
