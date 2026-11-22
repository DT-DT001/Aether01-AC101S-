#include "app_motion_filter.h"
#include <math.h>
#include <string.h>
#define G 9.80665f
#define RAD 0.01745329252f
#define DEG 57.29577951f
static float clamp(float v,float lo,float hi) { return v<lo?lo:v>hi?hi:v; }
static void normalize(float q[4]) {
    float n=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if(n>0.00001f) for(unsigned i=0;i<4;++i) q[i]/=n;
    else {q[0]=1; q[1]=q[2]=q[3]=0;}
}
static void angles(app_motion_filter_t *f) {
    const float *a=f->reference,*b=f->q;
    f->relative[0]=a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
    f->relative[1]=a[0]*b[1]-a[1]*b[0]-a[2]*b[3]+a[3]*b[2];
    f->relative[2]=a[0]*b[2]+a[1]*b[3]-a[2]*b[0]-a[3]*b[1];
    f->relative[3]=a[0]*b[3]-a[1]*b[2]+a[2]*b[1]-a[3]*b[0];
    normalize(f->relative);
    float w=f->relative[0],x=f->relative[1],y=f->relative[2],z=f->relative[3];
    f->roll=atan2f(2*(w*x+y*z),1-2*(x*x+y*y))*DEG;
    f->pitch=asinf(clamp(2*(w*y-z*x),-1,1))*DEG;
    f->yaw=atan2f(2*(w*z+x*y),1-2*(y*y+z*z))*DEG;
}
void app_motion_filter_init(app_motion_filter_t *f) {
    memset(f,0,sizeof(*f)); f->q[0]=f->reference[0]=f->relative[0]=1;
    f->action=APP_MOTION_WAITING;
}
void app_motion_filter_zero(app_motion_filter_t *f) {
    if(!f || !f->initialized) return;
    memcpy(f->reference,f->q,sizeof(f->reference)); angles(f);
}
static bool update_attitude(app_motion_filter_t *f,const float a[3],const float gyro[3],
                            uint32_t stamp,bool use_gyro) {
    if(!f || !a || !gyro) return false;
    for(unsigned i=0;i<3;++i) if(!isfinite(a[i]) || !isfinite(gyro[i]) || fabsf(a[i])>200 || fabsf(gyro[i])>2200) return false;
    if(f->initialized && stamp==f->stamp) return false;
    uint32_t elapsed=f->initialized ? stamp-f->stamp : 20;
    if(f->initialized && elapsed>0x7fffffffU) return false;
    float n=sqrtf(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
    float speed=sqrtf(gyro[0]*gyro[0]+gyro[1]*gyro[1]+gyro[2]*gyro[2]);
    const bool gravity_valid = n > G * .75f && n < G * 1.25f;
    if (!use_gyro && !gravity_valid) return false;
    if(!f->initialized || elapsed>250) {
        if(n<0.1f) return false;
        float roll=atan2f(a[1],a[2]),pitch=atan2f(-a[0],hypotf(a[1],a[2]));
        float cr=cosf(roll*.5f),sr=sinf(roll*.5f),cp=cosf(pitch*.5f),sp=sinf(pitch*.5f);
        f->q[0]=cr*cp;f->q[1]=sr*cp;f->q[2]=cr*sp;f->q[3]=-sr*sp;
        f->initialized=true;f->reversals=0;f->previous_sign=0;
        elapsed=0;
    }
    float dt=elapsed?clamp(elapsed*.001f,.001f,.1f):0;
    float w=f->q[0],x=f->q[1],y=f->q[2],z=f->q[3];
    float gravity[3]={2*(x*z-w*y),2*(w*x+y*z),1-2*(x*x+y*y)};
    float lin[3]={a[0]-gravity[0]*G,a[1]-gravity[1]*G,a[2]-gravity[2]*G};
    float linear=sqrtf(lin[0]*lin[0]+lin[1]*lin[1]+lin[2]*lin[2]);
    float rate[3] = {0};
    if (use_gyro) for (unsigned i = 0; i < 3; ++i) rate[i] = gyro[i] * RAD;
    if (gravity_valid) {
        /* When bias is unknown, follow gravity quickly without integrating drift. */
        const float correction = use_gyro ? 1.8f : fminf(22.0f, 0.9f / fmaxf(dt, 0.001f));
        float m[3]={a[0]/n,a[1]/n,a[2]/n};
        rate[0]+=correction*(m[1]*gravity[2]-m[2]*gravity[1]);
        rate[1]+=correction*(m[2]*gravity[0]-m[0]*gravity[2]);
        rate[2]+=correction*(m[0]*gravity[1]-m[1]*gravity[0]);
    }
    float dx=rate[0]*dt*.5f,dy=rate[1]*dt*.5f,dz=rate[2]*dt*.5f;
    f->q[0]=w-x*dx-y*dy-z*dz; f->q[1]=x+w*dx+y*dz-z*dy;
    f->q[2]=y+w*dy-x*dz+z*dx; f->q[3]=z+w*dz+x*dy-y*dx;
    normalize(f->q);angles(f);
    f->acceleration=n;f->rotation=speed;f->linear+=fminf(1,dt*12)*(linear-f->linear);
    float dominant=lin[0]; if(fabsf(lin[1])>fabsf(dominant)) dominant=lin[1]; if(fabsf(lin[2])>fabsf(dominant)) dominant=lin[2];
    int sign=dominant>2.8f?1:dominant< -2.8f?-1:0;
    if(stamp-f->shake_stamp>650) {f->reversals=0;f->previous_sign=0;}
    if(sign && sign!=f->previous_sign) {
        if(f->previous_sign) ++f->reversals;
        f->previous_sign=sign;f->shake_stamp=stamp;
    }
    if(n>G*1.9f) {f->impact_stamp=stamp;f->impact_active=true;}
    if(f->impact_active && stamp-f->impact_stamp>=350) f->impact_active=false;
    if(n<G*.25f) f->action=APP_MOTION_FREEFALL;
    else if(f->impact_active && stamp-f->impact_stamp<350) f->action=APP_MOTION_IMPACT;
    else if(f->reversals>=3 && stamp-f->shake_stamp<650) f->action=APP_MOTION_SHAKING;
    else if(speed>35) f->action=APP_MOTION_TURNING;
    else if(f->linear>1.8f) f->action=APP_MOTION_MOVING;
    else if(fabsf(f->roll)>15 || fabsf(f->pitch)>15) f->action=APP_MOTION_TILTED;
    else f->action=APP_MOTION_STILL;
    f->history[f->history_next]=clamp(f->linear,0,20);f->history_next=(f->history_next+1)%48;
    f->stamp=stamp;
    return true;
}
bool app_motion_filter_update(app_motion_filter_t *f,const float a[3],
                              const float gyro[3],uint32_t stamp) {
    return update_attitude(f,a,gyro,stamp,true);
}
bool app_motion_filter_update_tilt(app_motion_filter_t *f,const float a[3],
                                   const float gyro[3],uint32_t stamp,bool gyro_bias_valid) {
    return update_attitude(f,a,gyro,stamp,gyro_bias_valid);
}
const char *app_motion_action_name(app_motion_action_t action) {
    static const char *const names[]={"等待采样","静止","倾斜","旋转","移动","摇晃","冲击","失重"};
    return (unsigned)action<sizeof(names)/sizeof(names[0])?names[action]:names[0];
}
