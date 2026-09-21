/* Revision-21 paths and rotation on the revision-38 object cache: pixels,
 * lifecycle and adversarial input. Reuses the firmware stubs, message builders
 * and cache regression suite of shapes_host_test.c in one translation unit. */
#define main legacy_main
#include "shapes_host_test.c"
#undef main

static uint8_t commands[8192];
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
/* PUT a path object (id, version) with the current `commands[0..len)`; returns the reply status. */
static int put_path(uint32_t id,uint32_t ver,int rule,uint32_t len,int x,int y,int scale) {
    uint8_t r[8192+16];uint8_t *e=rec_path(r,15,(uint8_t)rule,x,y,scale,commands,len);
    m_begin(50);m_put_object(id,ver,r,(uint32_t)(e-r));
    if(send(37)!=0)return -1;return reply_status();
}
static int show_ids(uint16_t request,const uint32_t *ids,const uint32_t *vers,uint32_t n) {
    m_show(request,1,0);m_refs(ids,vers,n);return send(38)==0?reply_status():-1;
}
/* SHOW with KEEP and one ROTATE op */
static int rotate1(uint16_t request,uint32_t id,int32_t angle,int px,int py,uint16_t ms) {
    m_show(request,1|2,0);m_refs(0,0,0);m_rotate(id,angle,px,py,ms);return send(38)==0?reply_status():-1;
}
static void pixels_and_paths(const char *dir) {
    fresh();uint32_t len=boxpath(0,20,20,120,120,0);len=boxpath(len,50,50,60,60,0);
    CHECK(put_path(1,1,1,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);
    uint32_t id=1,ver=1;CHECK(show_ids(1,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);
    cfw_cache *sc=g_ctx.scene;
    CHECK(pixel_at(sc->fb,30,30)==15);CHECK(pixel_at(sc->fb,80,80)==0);
    CHECK(count_color(sc->fb,15)==120*120-60*60);
    write_pgm(dir,"path_evenodd",sc->fb);
    /* a new version of the same object replaces it on the list: nonzero fills the hole */
    CHECK(put_path(1,2,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);ver=2;
    CHECK(show_ids(2,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);CHECK(pixel_at(sc->fb,80,80)==15);
    len=boxpath(0,20,20,120,120,0);len=boxpath(len,50,50,60,60,1);
    CHECK(put_path(1,3,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);ver=3;
    CHECK(show_ids(3,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);CHECK(pixel_at(sc->fb,80,80)==0);
    /* Disjoint contours and negative coordinates. */
    len=boxpath(0,-20,-20,50,50,0);len=boxpath(len,200,200,20,20,0);
    CHECK(put_path(1,4,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);ver=4;
    CHECK(show_ids(4,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);CHECK(count_color(sc->fb,15)==900+400);
    /* Cubic bulge followed by a quadratic and closure. */
    len=point(0,0,100,200);commands[len++]=3;
    int v[]={100,40,240,40,240,200};for(int i=0;i<6;i++){put16(commands+len,v[i]*16);len+=2;}
    commands[len++]=2;int q[]={170,260,100,200};for(int i=0;i<4;i++){put16(commands+len,q[i]*16);len+=2;}
    commands[len++]=4;
    CHECK(put_path(1,5,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);ver=5;
    CHECK(show_ids(5,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);
    const cfw_path *path=(const cfw_path *)(const void *)(g_ctx.texture_cache+sc->assets[obj(1)->asset-1].offset);
    CHECK(path->count>8);
    CHECK(pixel_at(sc->fb,170,130)==15);CHECK(pixel_at(sc->fb,170,40)==0);
    write_pgm(dir,"path_curves",sc->fb);
    /* each replacement freed the previous private edge asset */
    CHECK(live_assets()==1);
}
static void advance(uint32_t ms) { g_now += ms - 33; tick(); }
static void rotations(const char *dir) {
    fresh();
    {uint8_t r[20];rec(r,CFW_SHAPE_RECT_FILL,15,0,(int16_t[]){100,100,80,40},4);m_begin(1);m_put_object(1,1,r,20);CHECK(send(37)==0);}
    uint32_t id=1,ver=1;CHECK(show_ids(1,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);cfw_cache *sc=g_ctx.scene;
    CHECK(rotate1(2,1,180*256,140,120,1000)==CFW_CACHE_REPLY_APPLIED);advance(500);
    CHECK(cv_abs(obj(1)->rotation.angle-90*256)<8);
    uint32_t area=count_color(sc->fb,15);CHECK(area>=3100 && area<=3300);
    CHECK(pixel_at(sc->fb,140,85)==15);CHECK(pixel_at(sc->fb,105,120)==0);
    write_pgm(dir,"rotation_halfway",sc->fb);
    advance(500);CHECK(obj(1)->rotation.angle==180*256);CHECK(!sc->anim_active);
    /* Full turn is not normalized away. */
    CHECK(rotate1(3,1,540*256,140,120,1000)==CFW_CACHE_REPLY_APPLIED);advance(500);
    CHECK(cv_abs(obj(1)->rotation.angle-360*256)<32);
    /* Retarget from the elapsed current angle while a glide runs. */
    m_show(4,1|2,0);m_refs(0,0,0);m_glide(1,40,0,10,LINEAR);m_rotate(1,720*256,140,120,1000);
    CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_APPLIED && obj(1)->frames==10);
    advance(100);CHECK(obj(1)->p[0]>100);CHECK(obj(1)->rotation.duration==1000);
    m_show(5,1|2,0);m_refs(0,0,0);m_op1(6,1);CHECK(send(38)==0);CHECK(!obj(1)->frames && !obj(1)->rotation.duration);
    int32_t frozen=obj(1)->rotation.angle;g_now+=1000;tick();CHECK(obj(1)->rotation.angle==frozen);
    CHECK(rotate1(6,1,-360*256,140,120,65535)==CFW_CACHE_REPLY_APPLIED);
    m_show(7,1|2,0);m_refs(0,0,0);m_op1(7,1);CHECK(send(38)==0);CHECK(obj(1)->rotation.angle==-360*256);
    /* Clock wraps and a skipped display tick catches up. */
    g_now=0xfffffff0u;CHECK(rotate1(8,1,0,140,120,100)==CFW_CACHE_REPLY_APPLIED);
    advance(120);CHECK(obj(1)->rotation.angle==0 && !sc->anim_active);
    /* Every primitive, rounded corners included, has a transformed path. */
    for(int type=1;type<=13;type++) {
        int16_t p[8]={100,100,60,40,15,140,180,220};
        if(type>=6 && type<=11){int16_t pts[]={100,100,160,110,140,160,190,170};for(int i=0;i<8;i++)p[i]=pts[i];}
        if(type==12 || type==13){p[3]=0;p[4]=250;}
        uint8_t r[20];rec(r,(uint8_t)type,15,3,p,8);
        m_show(9,1,0);m_put_object(10+type,1,r,20);m_put_count(1);
        uint32_t o=10+type,w=1;m_refs(&o,&w,1);m_rotate(o,37*256,140,140,0);
        CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_APPLIED);
        CHECK(count_color(sc->fb,15)>0);
    }
    /* Path movement/scale/color are regular tween parameters. */
    uint32_t len=boxpath(0,0,0,40,20,0);CHECK(put_path(30,1,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);
    {uint32_t o=30,w=1;m_show(10,1,0);m_refs(&o,&w,1);int16_t vals[]={100,100,512,7};m_tween(30,0x107,2,LINEAR,vals);m_rotate(30,90*256,100,100,66);
     CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_APPLIED);}
    tick();tick();
    CHECK(obj(30)->p[0]==100 && obj(30)->p[2]==512);
    CHECK(pixel_at(sc->fb,80,140)==7);
    write_pgm(dir,"path_transform",sc->fb);
}
static void invalid_and_lifecycle(void) {
    fresh();uint32_t len=boxpath(0,10,10,30,30,0);
    CHECK(put_path(1,1,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);
    uint32_t id=1,ver=1;CHECK(show_ids(1,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);cfw_cache *sc=g_ctx.scene;
    uint16_t old_asset=obj(1)->asset;uint32_t before=count_color(sc->fb,15);int live=g_alloc_live;uint32_t assets=live_assets(),used=store_used();
    /* A valid replacement followed by a bad entry must not apply. */
    {uint8_t r[64];uint8_t *e=rec_path(r,15,0,0,0,256,commands,len);m_begin(2);m_put_object(1,2,r,(uint32_t)(e-r));g_msg[g_len++]=99;g_msg[g_len++]=0;
     CHECK(send(37)==-1);CHECK(obj(1)->asset==old_asset && obj(1)->version==1 && count_color(sc->fb,15)==before);
     uint32_t whole=g_len-2;for(uint32_t n=5;n<whole;n++){g_len=n;CHECK(send(37)==-1);}
     CHECK(g_alloc_live==live && live_assets()==assets && store_used()==used);}
    /* Invalid command, wrong length, bad winding rule, duplicate id, allocation failures. */
    commands[5]=99;CHECK(put_path(1,2,0,len,0,0,256)==CFW_CACHE_REPLY_REFUSED);commands[5]=1;
    {uint8_t r[64];uint8_t *e=rec_path(r,15,0,0,0,256,commands,len);put16(r+10,(int)len-1);m_begin(3);m_put_object(1,2,r,(uint32_t)(e-r));CHECK(send(37)==-1);}
    CHECK(put_path(1,2,2,len,0,0,256)==CFW_CACHE_REPLY_REFUSED);
    CHECK(put_path(1,2,0,len,0,0,2049)==CFW_CACHE_REPLY_REFUSED);
    {uint8_t r[64];uint8_t *e=rec_path(r,15,0,0,0,256,commands,len);m_begin(4);m_put_object(1,3,r,(uint32_t)(e-r));m_put_object(1,3,r,(uint32_t)(e-r));
     CHECK(send(37)==0 && reply_status()==CFW_CACHE_REPLY_REFUSED && reply_extra()[0]==CFW_CACHE_REFUSE_DUPLICATE);}
    CHECK(g_alloc_live==live && live_assets()==assets && store_used()==used && obj(1)->version==1);
    /* a store that cannot be allocated: CAPACITY */
    {cfw_scene_release(&g_ctx);bzero((uint8_t *)&g_ctx,sizeof g_ctx);g_ctx.magic=CFW_CTX_MAGIC;g_epoch=0;g_store_fail=1;m_begin(1);g_msg[g_len++]=0;CHECK(send(41)==0);g_epoch=reply_epoch();
     CHECK(put_path(1,1,0,len,0,0,256)==CFW_CACHE_REPLY_CAPACITY);g_store_fail=0;CHECK(live_objects()==0 && live_assets()==0);
     CHECK(put_path(1,1,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);CHECK(show_ids(1,&id,&ver,1)==CFW_CACHE_REPLY_APPLIED);sc=g_ctx.scene;before=count_color(sc->fb,15);}
    /* Wrong op target type / absent object / extreme angle / path tween limits. */
    {uint8_t r[20];rec(r,CFW_SHAPE_IMAGE,15,0,(int16_t[]){0,0},2);(void)r;}
    CHECK(rotate1(5,2,0,0,0,100)==CFW_CACHE_REPLY_REFUSED);
    {m_show(6,1|2,0);m_refs(0,0,0);m_rotate(1,2147483647,0,0,100);CHECK(send(38)==-1);}
    {int16_t v[]={0};m_show(7,1|2,0);m_refs(0,0,0);m_tween(1,8,2,LINEAR,v);CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_REFUSED && reply_extra()[0]==CFW_CACHE_REFUSE_MASK);}  /* paths have no p3 */
    {int16_t v[]={-1};m_show(8,1|2,0);m_refs(0,0,0);m_tween(1,4,2,LINEAR,v);CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_REFUSED);}
    {int16_t v[]={2049};m_show(9,1|2,0);m_refs(0,0,0);m_tween(1,4,2,LINEAR,v);CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_REFUSED);}
    {int16_t v[]={16};m_show(10,1|2,0);m_refs(0,0,0);m_tween(1,256,2,LINEAR,v);CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_REFUSED);}
    CHECK(count_color(sc->fb,15)==before);
    /* Too many edges in one path. */
    len=point(0,0,0,0);for(int i=0;i<513;i++)len=point(len,1,i%2?100:0,i+1);commands[len++]=4;
    CHECK(put_path(2,1,0,len,0,0,256)==CFW_CACHE_REPLY_REFUSED);CHECK(obj(2)==0);
    /* Exactly 512 edges each; eight fit the active budget, a ninth on the list is refused atomically. */
    len=point(0,0,0,0);for(int i=0;i<511;i++)len=point(len,1,i%2?100:0,i+1);commands[len++]=4;
    uint32_t ids[9],vers[9];
    for(uint32_t i=0;i<9;i++){CHECK(put_path(100+i,1,0,len,0,0,256)==CFW_CACHE_REPLY_APPLIED);ids[i]=100+i;vers[i]=1;}
    CHECK(show_ids(11,ids,vers,8)==CFW_CACHE_REPLY_APPLIED);
    CHECK(show_ids(12,ids,vers,9)==CFW_CACHE_REPLY_REFUSED && reply_extra()[0]==CFW_CACHE_REFUSE_EDGES && sc->active_count==8);
    /* Lease expiry prevents writes and stops timers. */
    CHECK(rotate1(13,100,180*256,50,50,1000)==CFW_CACHE_REPLY_APPLIED);
    g_lease=0;CHECK(rotate1(14,100,0,50,50,0)==-1);tick();CHECK(!sc->anim_active);g_lease=1;
    /* No shadow at all: the SHOW fails instead of rendering anywhere. */
    g_shadow_missing=1;CHECK(show_ids(15,ids,vers,1)==-1);g_shadow_missing=0;
    /* A timer that cannot be created snaps the rotation to its end. */
    g_ctx.scene_timer=0;g_fail_timer=1;
    CHECK(rotate1(16,100,90*256,30,30,1000)==CFW_CACHE_REPLY_APPLIED);
    CHECK(!sc->anim_active && obj(100)->rotation.angle==90*256);g_fail_timer=0;
    fresh();
}
static void fuzz(void) {
    fresh();cfw_vector_work *work=malloc(sizeof(*work));CHECK(work!=0);
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
        uint8_t r[20];rec(r,(uint8_t)(i%13+1),15,255,p,8);
        m_show(20,1,0);m_put_object(500,(uint32_t)i+1,r,20);m_put_count(1);
        uint32_t o=500,w=(uint32_t)i+1;m_refs(&o,&w,1);m_rotate(500,37*256,p[0],p[1],0);
        CHECK(send(38)==0 && reply_status()==CFW_CACHE_REPLY_APPLIED);
    }
    /* Random cache messages must never crash or leak; the reply is either a
     * status or the message is NACKed. */
    for(int i=0;i<20000;i++) {
        uint32_t len=(uint32_t)i%200+1;
        for(uint32_t k=0;k<len;k++){seed=seed*1664525u+1013904223u;g_msg[k]=(uint8_t)(seed>>24);}
        if(len>=4){seed=seed*1664525u+1013904223u;if(seed&1)put16(g_msg,g_epoch);}
        g_len=len;uint8_t mode=(uint8_t)(37+(seed>>8)%5);
        int r=send(mode);
        CHECK(r==0 || r==-1);
        if(r==0 && mode==41 && g_len>=5 && g_msg[4]==0)g_epoch=reply_epoch();
        if(r==0 && reply_status()==CFW_CACHE_REPLY_STALE)g_epoch=reply_epoch();
    }
    CHECK(store_used()<=CFW_CACHE_STORE_BYTES);
}
/* Cache messages in a dump carry epoch 0 and request 0: the sender fills in the
 * live epoch and a fresh request id, as demos/cfw-transport.ts CacheLink does. */
static uint16_t g_replay_request;
static int replay_message(const uint8_t *data,uint32_t len,int present) {
    cfw_rectlist rl={0};g_ctx.direct_pending=0;g_ctx.cache_reply_cap=245;g_ctx.cache_reply_len=0;
    uint8_t mode=data[0];
    if(mode>=37 && mode<=41 && len>=5 && rd16(data+1)==0) {
        for(uint32_t i=0;i<len && i<sizeof g_msg;i++)g_msg[i]=data[i];
        put16(g_msg+1,g_epoch);
        if(mode!=40 && rd16(data+3)==0){if(++g_replay_request==0)g_replay_request=1;put16(g_msg+3,g_replay_request);}
        int r=cfw_scene_dispatch(&g_ctx,mode,g_msg+1,len-1,present,&rl);
        if(getenv("REPLAY_DEBUG")) {
            cfw_cache *sc=g_ctx.scene;
            fprintf(stderr,"  mode %u r %d status %d active %u",mode,r,g_ctx.cache_reply_len>=10?g_ctx.cache_reply[3]:-1,sc?sc->active_count:0);
            for(uint32_t i=0;sc && i<sc->active_count;i++){const cfw_object *o=&sc->objects[sc->active[i]];fprintf(stderr," [%u:id%u v%08x t%u a%u]",sc->active[i],o->id,o->version,o->type,o->asset);}
            fprintf(stderr,"\n");
        }
        if(r==0 && g_ctx.cache_reply_len>=10) {
            uint8_t status=g_ctx.cache_reply[3];
            if(mode==41 && g_msg[5]==0)g_epoch=reply_epoch();
            if(status!=CFW_CACHE_REPLY_APPLIED && status!=CFW_CACHE_REPLY_UNCHANGED && status!=CFW_CACHE_REPLY_DELTA && status!=CFW_CACHE_REPLY_SNAPSHOT)
                return 100+status;
        }
        return r;
    }
    return cfw_scene_dispatch(&g_ctx,mode,data+1,len-1,present,&rl);
}
static int replay(const char *file,const char *dir) {
    FILE *f=fopen(file,"rb");if(!f){perror(file);return 1;}
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    if(size<0 || size>64*1024*1024){fclose(f);return 1;}
    uint8_t *data=malloc((size_t)size);if(!data){fclose(f);return 1;}
    if(fread(data,1,(size_t)size,f)!=(size_t)size){fclose(f);free(data);return 1;}fclose(f);
    fresh();uint32_t pos=0,messages=0,probes=0;char label[260]="fixture";
    while(pos<(uint32_t)size) {
        uint8_t kind=data[pos++];uint32_t avail=(uint32_t)size-pos;
        if(kind==1) {
            if(avail<2)goto malformed;
            uint32_t len=rd16(data+pos);pos+=2;
            if(len<2 || len>(uint32_t)size-pos)goto malformed;
            cfw_cache_prepare(&g_ctx,data[pos]);          /* image_worker does this before the gate */
            int result=replay_message(data+pos,len,1);
            if(result){printf("REJECTED %s, message %u (mode %u, result %d)\n",label,messages,data[pos],result);g_fail++;}
            pos+=len;messages++;
        } else if(kind==2) {
            if(avail<4)goto malformed;
            uint32_t ms=rd32(data+pos);pos+=4;if(ms>60000)goto malformed;
            while(ms){uint32_t step=ms<33?ms:33;g_now+=step-33;tick();ms-=step;}
        } else if(kind==3) {
            if(avail<1)goto malformed;
            uint32_t n=data[pos++];if(n>(uint32_t)size-pos)goto malformed;
            for(uint32_t i=0;i<n;i++)label[i]=(char)data[pos+i];label[n]=0;pos+=n;
        } else if(kind==4) {
            if(avail<5)goto malformed;
            uint32_t x=rd16(data+pos),y=rd16(data+pos+2),color=data[pos+4];pos+=5;
            if(x>=IMAGE_W || y>=IMAGE_H || color>15)goto malformed;
            cfw_cache *sc=g_ctx.scene;
            if(!sc || !sc->fb || pixel_at(sc->fb,(int)x,(int)y)!=color) {
                printf("PIXEL FAIL %s (%u,%u) expected %u\n",label,x,y,color);g_fail++;
            }
            probes++;
        } else if(kind==5) {
            cfw_cache *sc=g_ctx.scene;
            /* Fixture labels are not used as file paths. */
            if(sc && sc->fb){char name[40];snprintf(name,sizeof name,"fixture_%03u",messages);write_pgm(dir,name,sc->fb);}
        } else goto malformed;
    }
    free(data);cfw_scene_release(&g_ctx);printf("replay: %u messages, %u pixel probes, %d failures\n",messages,probes,g_fail);
    return g_fail?1:0;
malformed:
    fprintf(stderr,"Malformed replay at byte %u\n",pos);free(data);cfw_scene_release(&g_ctx);return 1;
}
int main(int argc,char **argv) {
    const char *dir=argc>1?argv[1]:".";
    if(argc>2)return replay(argv[2],dir);
    CHECK(legacy_main(argc,argv)==0);
    pixels_and_paths(dir);rotations(dir);invalid_and_lifecycle();fuzz();
    cfw_scene_release(&g_ctx);
    printf("vector suite: %s (%d failures), %d live allocations\n",g_fail?"FAILED":"OK",g_fail,g_alloc_live);
    return (g_fail || g_alloc_live)?1:0;
}
