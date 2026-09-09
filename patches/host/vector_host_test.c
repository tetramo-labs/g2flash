/* Revision-21 protocol, pixels, lifecycle and adversarial-input tests.
 * Reuse the firmware stubs and legacy regression suite in one translation unit. */
#define main legacy_main
#include "shapes_host_test.c"
#undef main

static uint8_t msg[65536], commands[8192];
static void put32(uint8_t *p,int32_t v){put16(p,v);put16(p+2,(uint32_t)v>>16);}
static uint32_t point(uint32_t at,uint8_t op,int x,int y) {
    commands[at]=op;put16(commands+at+1,x*16);put16(commands+at+3,y*16);return at+5;
}
static uint32_t boxpath(uint32_t at,int x,int y,int w,int h,int reverse) {
    at=point(at,0,x,y);
    at=point(at,1,reverse?x:x+w,reverse?y+h:y);
    at=point(at,1,x+w,y+h);
    at=point(at,1,reverse?x+w:x,reverse?y:y+h);
    commands[at++]=4;return at;
}
static uint32_t pathop(uint32_t at,uint8_t slot,int rule,uint32_t len) {
    uint8_t *p=msg+at;p[0]=8;p[1]=slot;p[2]=1;p[3]=15;p[4]=(uint8_t)rule;
    put16(p+5,0);put16(p+7,0);put16(p+9,256);put16(p+11,len);
    for(uint32_t i=0;i<len;i++)p[13+i]=commands[i];return at+13+len;
}
static uint32_t rotateop(uint32_t at,uint8_t slot,int32_t angle,int px,int py,uint16_t ms) {
    uint8_t *p=msg+at;p[0]=9;p[1]=slot;put32(p+2,angle);put16(p+6,px);put16(p+8,py);
    put16(p+10,ms);p[12]=p[13]=0;p[14]=p[15]=255;return at+16;
}
static int dispatch(uint32_t len) {
    cfw_rectlist rl={0};g_ctx.direct_pending=0;
    return cfw_scene_dispatch(&g_ctx,g_container_shadow,37,msg,len,1,&rl);
}
static void reset(void) {
    g_alloc_fail_after=-1;g_fail_frame_alloc=0;g_fail_timer=0;g_lease=1;
    cfw_scene_release(&g_ctx);CHECK(g_alloc_live==0);
    g_ctx.magic=CFW_CTX_MAGIC;g_now=0;msg[0]=3;msg[1]=0;
}
static void tick(uint32_t ms) {
    g_now+=ms;g_ctx.direct_pending=0;scene_tick(&g_ctx);
    cfw_scene *sc=cfw_scene_peek(&g_ctx);if(sc)cfw_scene_render_if_due(&g_ctx,sc->fb);
}
static void pixels_and_paths(const char *dir) {
    reset();uint32_t len=boxpath(0,20,20,120,120,0);len=boxpath(len,50,50,60,60,0);
    CHECK(dispatch(pathop(2,0,1,len))==0);
    cfw_scene *sc=g_ctx.scene;
    CHECK(pixel_at(sc->fb,30,30)==15);CHECK(pixel_at(sc->fb,80,80)==0);
    CHECK(count_color(sc->fb,15)==120*120-60*60);
    write_pgm(dir,"path_evenodd",sc->fb);
    CHECK(dispatch(pathop(2,0,0,len))==0);CHECK(pixel_at(sc->fb,80,80)==15);
    len=boxpath(0,20,20,120,120,0);len=boxpath(len,50,50,60,60,1);
    CHECK(dispatch(pathop(2,0,0,len))==0);CHECK(pixel_at(sc->fb,80,80)==0);
    /* Disjoint contours and negative coordinates. */
    len=boxpath(0,-20,-20,50,50,0);len=boxpath(len,200,200,20,20,0);
    CHECK(dispatch(pathop(2,0,0,len))==0);CHECK(count_color(sc->fb,15)==900+400);
    /* Cubic bulge followed by a quadratic and closure. */
    len=point(0,0,100,200);commands[len++]=3;
    int v[]={100,40,240,40,240,200};for(int i=0;i<6;i++){put16(commands+len,v[i]*16);len+=2;}
    commands[len++]=2;int q[]={170,260,100,200};for(int i=0;i<4;i++){put16(commands+len,q[i]*16);len+=2;}
    commands[len++]=4;
    CHECK(dispatch(pathop(2,0,0,len))==0);CHECK(sc->slots[0].path->count>8);
    CHECK(pixel_at(sc->fb,170,130)==15);CHECK(pixel_at(sc->fb,170,40)==0);
    write_pgm(dir,"path_curves",sc->fb);
}
static void rotations(const char *dir) {
    reset();msg[2]=0;msg[3]=0;
    rec(msg+4,CFW_SHAPE_RECT_FILL,15,0,(int16_t[]){100,100,80,40},4);
    CHECK(dispatch(24)==0);cfw_scene *sc=g_ctx.scene;
    msg[0]=1;uint32_t end=rotateop(2,0,180*256,140,120,1000);
    CHECK(dispatch(end)==0);tick(500);
    CHECK(cv_abs(sc->slots[0].rotation.angle-90*256)<8);
    uint32_t area=count_color(sc->fb,15);CHECK(area>=3100 && area<=3300);
    CHECK(pixel_at(sc->fb,140,85)==15);CHECK(pixel_at(sc->fb,105,120)==0);
    write_pgm(dir,"rotation_halfway",sc->fb);
    tick(500);CHECK(sc->slots[0].rotation.angle==180*256);CHECK(!sc->anim_active);
    /* Full turn is not normalized away. */
    CHECK(dispatch(rotateop(2,0,540*256,140,120,1000))==0);tick(500);
    CHECK(cv_abs(sc->slots[0].rotation.angle-360*256)<32);
    /* Retarget from elapsed current angle, while retaining geometry animation. */
    msg[2]=4;msg[3]=0;put16(msg+4,40);put16(msg+6,0);msg[8]=10;msg[9]=msg[10]=0;msg[11]=msg[12]=255;
    CHECK(dispatch(rotateop(13,0,720*256,140,120,1000))==0);CHECK(sc->slots[0].frames==10);
    tick(100);CHECK(sc->slots[0].p[0]>100);CHECK(sc->slots[0].rotation.duration==1000);
    msg[2]=6;msg[3]=0;CHECK(dispatch(4)==0);CHECK(!sc->slots[0].frames && !sc->slots[0].rotation.duration);
    int32_t frozen=sc->slots[0].rotation.angle;tick(1000);CHECK(sc->slots[0].rotation.angle==frozen);
    CHECK(dispatch(rotateop(2,0,-360*256,140,120,65535))==0);
    msg[2]=7;msg[3]=0;CHECK(dispatch(4)==0);CHECK(sc->slots[0].rotation.angle==-360*256);
    /* Clock wraps and a skipped display tick catches up. */
    g_now=0xfffffff0u;CHECK(dispatch(rotateop(2,0,0,140,120,100))==0);
    tick(120);CHECK(sc->slots[0].rotation.angle==0 && !sc->anim_active);
    /* Every primitive, rounded corners included, has a transformed path. */
    for(int type=1;type<=13;type++) {
        int16_t p[8]={100,100,60,40,15,140,180,220};
        if(type>=6 && type<=11){int16_t pts[]={100,100,160,110,140,160,190,170};for(int i=0;i<8;i++)p[i]=pts[i];}
        if(type==12 || type==13){p[3]=0;p[4]=250;}
        msg[0]=3;msg[2]=0;msg[3]=0;rec(msg+4,(uint8_t)type,15,3,p,8);
        CHECK(dispatch(rotateop(24,0,37*256,140,140,0))==0);
        CHECK(count_color(sc->fb,15)>0);
    }
    /* Path movement/scale/color are regular legacy tween parameters. */
    uint32_t len=boxpath(0,0,0,40,20,0);CHECK(dispatch(pathop(2,0,0,len))==0);
    msg[0]=1;msg[2]=5;msg[3]=0;put16(msg+4,0x107);msg[6]=2;msg[7]=msg[8]=0;msg[9]=msg[10]=255;
    put16(msg+11,100);put16(msg+13,100);put16(msg+15,512);put16(msg+17,7);
    CHECK(dispatch(rotateop(19,0,90*256,100,100,66))==0);tick(33);tick(33);
    CHECK(sc->slots[0].p[0]==100 && sc->slots[0].p[2]==512);
    CHECK(pixel_at(sc->fb,80,140)==7);
    write_pgm(dir,"path_transform",sc->fb);
}
static void invalid_and_lifecycle(void) {
    reset();uint32_t len=boxpath(0,10,10,30,30,0);uint32_t end=pathop(2,0,0,len);
    CHECK(dispatch(end)==0);cfw_scene *sc=g_ctx.scene;cfw_path *old=sc->slots[0].path;
    uint32_t before=count_color(sc->fb,15);int live=g_alloc_live;
    /* CLEAR and a valid prefix must not apply before a bad tail validates. */
    msg[end]=99;msg[end+1]=0;CHECK(dispatch(end+2)==-1);
    CHECK(sc->slots[0].path==old && count_color(sc->fb,15)==before);
    for(uint32_t n=3;n<end;n++)CHECK(dispatch(n)==-1);
    CHECK(g_alloc_live==live);
    /* Invalid command/closure/winding, duplicate replacement, and allocation failures. */
    msg[15]=99;CHECK(dispatch(end)==-1);pathop(2,0,0,len);
    put16(msg+13,len-1);CHECK(dispatch(end-1)==-1);pathop(2,0,0,len);
    msg[6]=2;CHECK(dispatch(end)==-1);pathop(2,0,0,len);
    CHECK(dispatch(pathop(end,0,0,len))==-1);CHECK(g_alloc_live==live);
    pathop(2,0,0,len);g_alloc_fail_after=0;CHECK(dispatch(end)==-1);g_alloc_fail_after=-1;
    CHECK(sc->slots[0].path==old && g_alloc_live==live);
    /* A second staged allocation fails; the first is reclaimed. */
    uint32_t both=pathop(end,1,0,len);g_alloc_fail_after=1;CHECK(dispatch(both)==-1);g_alloc_fail_after=-1;
    CHECK(sc->slots[0].path==old && g_alloc_live==live);
    /* Wrong target type / unknown slot / extreme angle. */
    msg[0]=1;CHECK(dispatch(rotateop(2,1,0,0,0,100))==-1);
    CHECK(dispatch(rotateop(2,0,2147483647,0,0,100))==-1);
    msg[2]=5;msg[3]=0;put16(msg+4,8);msg[6]=2;msg[7]=msg[8]=0;msg[9]=msg[10]=255;put16(msg+11,0);
    CHECK(dispatch(13)==-1); /* paths have no p3 tween parameter */
    put16(msg+4,4);put16(msg+11,-1);CHECK(dispatch(13)==-1);
    put16(msg+11,2049);CHECK(dispatch(13)==-1);
    put16(msg+4,256);put16(msg+11,16);CHECK(dispatch(13)==-1);
    CHECK(sc->slots[0].path==old && count_color(sc->fb,15)==before);
    /* Too many edges. */
    len=point(0,0,0,0);for(int i=0;i<513;i++)len=point(len,1,i%2?100:0,i+1);commands[len++]=4;
    CHECK(dispatch(pathop(2,0,0,len))==-1);CHECK(sc->slots[0].path==old);
    /* Exactly 512 edges; eight such paths fit, a ninth is rejected atomically. */
    len=point(0,0,0,0);for(int i=0;i<511;i++)len=point(len,1,i%2?100:0,i+1);commands[len++]=4;
    msg[0]=3;end=2;for(int i=0;i<8;i++)end=pathop(end,(uint8_t)i,0,len);
    CHECK(dispatch(end)==0);old=sc->slots[0].path;live=g_alloc_live;
    msg[0]=1;CHECK(dispatch(pathop(2,8,0,len))==-1);CHECK(sc->slots[0].path==old && g_alloc_live==live);
    /* Lease expiry prevents writes and stops timers. */
    CHECK(dispatch(rotateop(2,0,180*256,50,50,1000))==0);
    g_lease=0;CHECK(dispatch(rotateop(2,0,0,50,50,0))==-1);tick(33);CHECK(!sc->anim_active);
    reset();
    /* No framebuffer: static final state survives in the container shadow. */
    g_fail_frame_alloc=1;len=boxpath(0,20,20,20,20,0);end=pathop(2,0,0,len);
    CHECK(dispatch(rotateop(end,0,90*256,30,30,1000))==0);
    CHECK(g_ctx.scene->fb==0 && !g_ctx.scene->slots[0].rotation.duration);
    CHECK(pixel_at(g_container_shadow,30,30)==15);reset();
    g_ctx.scene_timer=0;g_fail_timer=1;
    end=pathop(2,0,0,len);CHECK(dispatch(rotateop(end,0,90*256,30,30,1000))==0);
    CHECK(!g_ctx.scene->anim_active && g_ctx.scene->slots[0].rotation.angle==90*256);
    reset();
}
static void fuzz(void) {
    reset();cfw_vector_work *work=malloc(sizeof(*work));CHECK(work!=0);
    uint32_t seed=0x12345678;
    for(int k=0;k<10000;k++) {
        uint32_t len=(uint32_t)k%256+1;
        for(uint32_t i=0;i<len;i++){seed=seed*1664525u+1013904223u;commands[i]=(uint8_t)(seed>>24);}
        int n=cv_compile(work,commands,len);CHECK(n>=-1 && n<=(int)CFW_PATH_MAX_EDGES);
    }
    free(work);
    /* Exercise wide intermediates against an independent native int64 division. */
    for(int i=0;i<10000;i++) {
        seed=seed*1664525u+1013904223u;int32_t a=(int32_t)(seed%2000000)-1000000;
        seed=seed*1664525u+1013904223u;int32_t b=(int32_t)(seed%2000000)-1000000;
        int32_t d=2000001;int64_t product=(int64_t)a*b;
        int64_t expected=product/d;if(product%d<0)expected--;
        CHECK(cv_muldiv(a,b,d)==expected);
    }
    /* Extreme signed wire coordinates and pivots exercise the large-number
     * clipping path under UBSan, not only random rejected command bytes. */
    for(int i=0;i<100;i++) {
        int16_t p[8];for(int k=0;k<8;k++){seed=seed*1664525u+1013904223u;p[k]=(int16_t)(seed>>16);}
        msg[0]=3;msg[2]=0;msg[3]=0;rec(msg+4,(uint8_t)(i%13+1),15,255,p,8);
        CHECK(dispatch(rotateop(24,0,37*256,p[0],p[1],0))==0);
    }
}
static int replay(const char *file,const char *dir) {
    FILE *f=fopen(file,"rb");if(!f){perror(file);return 1;}
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    if(size<0 || size>64*1024*1024){fclose(f);return 1;}
    uint8_t *data=malloc((size_t)size);if(!data){fclose(f);return 1;}
    if(fread(data,1,(size_t)size,f)!=(size_t)size){fclose(f);free(data);return 1;}fclose(f);
    reset();uint32_t pos=0,messages=0,probes=0;char label[260]="fixture";
    while(pos<(uint32_t)size) {
        uint8_t kind=data[pos++];uint32_t avail=(uint32_t)size-pos;
        if(kind==1) {
            if(avail<2)goto malformed;
            uint32_t len=rd16(data+pos);pos+=2;
            if(len<2 || len>(uint32_t)size-pos)goto malformed;
            cfw_rectlist rl={0};g_ctx.direct_pending=0;
            int result=cfw_scene_dispatch(&g_ctx,g_container_shadow,data[pos],data+pos+1,len-1,1,&rl);
            if(result){printf("REJECTED %s, message %u\n",label,messages);g_fail++;}
            pos+=len;messages++;
        } else if(kind==2) {
            if(avail<4)goto malformed;
            uint32_t ms=rd32(data+pos);pos+=4;if(ms>60000)goto malformed;
            while(ms){uint32_t step=ms<33?ms:33;tick(step);ms-=step;}
        } else if(kind==3) {
            if(avail<1)goto malformed;
            uint32_t n=data[pos++];if(n>(uint32_t)size-pos)goto malformed;
            for(uint32_t i=0;i<n;i++)label[i]=(char)data[pos+i];label[n]=0;pos+=n;
        } else if(kind==4) {
            if(avail<5)goto malformed;
            uint32_t x=rd16(data+pos),y=rd16(data+pos+2),color=data[pos+4];pos+=5;
            if(x>=IMAGE_W || y>=IMAGE_H || color>15)goto malformed;
            cfw_scene *sc=g_ctx.scene;
            if(!sc || !sc->fb || pixel_at(sc->fb,(int)x,(int)y)!=color) {
                printf("PIXEL FAIL %s (%u,%u) expected %u\n",label,x,y,color);g_fail++;
            }
            probes++;
        } else if(kind==5) {
            cfw_scene *sc=g_ctx.scene;
            /* Fixture labels are not used as file paths. */
            if(sc && sc->fb){char name[40];snprintf(name,sizeof name,"fixture_%03u",messages);write_pgm(dir,name,sc->fb);}
        } else goto malformed;
    }
    free(data);reset();printf("replay: %u messages, %u pixel probes, %d failures\n",messages,probes,g_fail);
    return g_fail?1:0;
malformed:
    fprintf(stderr,"Malformed replay at byte %u\n",pos);free(data);reset();return 1;
}
int main(int argc,char **argv) {
    const char *dir=argc>1?argv[1]:".";
    if(argc>2)return replay(argv[2],dir);
    CHECK(legacy_main(argc,argv)==0);
    pixels_and_paths(dir);rotations(dir);invalid_and_lifecycle();fuzz();reset();
    printf("vector suite: %s (%d failures), %d live allocations\n",g_fail?"FAILED":"OK",g_fail,g_alloc_live);
    return g_fail?1:0;
}
