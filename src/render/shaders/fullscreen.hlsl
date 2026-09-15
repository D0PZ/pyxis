// ============================================================================
//  fullscreen.hlsl - Vertex shader del triangulo a pantalla completa
//
//  No hay bufer de vertices ni de indices: la geometria se genera a partir de
//  SV_VertexID con un Draw(3, 0). Es la tecnica estandar y evita reservar,
//  enlazar y validar buferes para dibujar lo que en el fondo es un rectangulo.
//
//  Se usa un TRIANGULO que sobresale de la pantalla, no dos triangulos que
//  forman un cuadrado. La diferencia importa: en el borde diagonal donde se
//  tocarian los dos triangulos, la GPU procesa los quads de pixeles dos veces.
//  Con un solo triangulo esa costura no existe.
//
//      id=0 -> uv(0,0)  pos(-1,  1)
//      id=1 -> uv(2,0)  pos( 3,  1)
//      id=2 -> uv(0,2)  pos(-1, -3)
//
//  El recorte descarta lo que sobra sin coste adicional.
// ============================================================================

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID) {
    VertexOutput output;

    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = uv;

    // El eje Y se invierte porque las coordenadas de textura crecen hacia abajo
    // y las de recorte hacia arriba.
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);

    return output;
}
