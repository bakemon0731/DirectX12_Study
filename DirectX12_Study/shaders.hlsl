

struct VSOutput
{
    float4 pos : SV_POSITION; // クリップ空間座標
    float4 color : COLOR;
};

// 頂点シェーダ
VSOutput VSMain(float3 pos : POSITION, float4 color : COLOR)
{
    VSOutput o;
    o.pos = float4(pos, 1.0f); 
    o.color = color;
    return o;
}

// ピクセルシェーダ
float4 PSMain(VSOutput i) : SV_TARGET
{
    return i.color;
}