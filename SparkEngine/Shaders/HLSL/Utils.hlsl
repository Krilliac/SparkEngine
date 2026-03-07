#ifndef UTILS_H
#define UTILS_H
static const float PI=3.14159265359;
static const float HALF_PI=1.57079632679;
float3 GammaToLinear(float3 c){return pow(c,2.2);}
float3 LinearToGamma(float3 c){return pow(c,1/2.2);}
float Remap(float v,float a,float b,float c,float d){return c+(v-a)*(d-c)/(b-a);}
float2 RotateUV(float2 uv,float angle){float ca=cos(angle),sa=sin(angle);float2 c=uv-0.5;return float2(c.x*ca-c.y*sa,c.x*sa+c.y*ca)+0.5;}
#endif
