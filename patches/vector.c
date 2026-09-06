#include "vector.h"

/* Checked path compiler and transformed contour rasterizer. Included by scene.c
 * after shapes.c, sharing its clipped span/line primitives and trig table. */

static int32_t cv_abs(int32_t v) { return v < 0 ? -v : v; }

/* Signed floor(a*b/d), d positive. Ordinary panel geometry takes hardware
 * 32-bit division; large offscreen coordinates use bounded long division,
 * avoiding an unresolved compiler-runtime 64-bit division call. */
static int32_t cv_muldiv(int32_t a, int32_t b, int32_t d) {
    int64_t n = (int64_t)a * b;
    if (n >= -2147483647LL && n <= 2147483647LL)
        return cfw_div_floor((int32_t)n, d);
    uint64_t u = n < 0 ? (uint64_t)(-n) : (uint64_t)n;
    uint64_t q = 0, rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((u >> i) & 1u);
        if (rem >= (uint32_t)d) { rem -= (uint32_t)d; q |= (uint64_t)1 << i; }
    }
    return n < 0 ? -(int32_t)q - (rem != 0) : (int32_t)q;
}

static int cv_edge(cfw_vector_work *w, uint32_t *n,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (x0 == x1 && y0 == y1) return 1;
    if (*n >= CFW_PATH_MAX_EDGES) return 0;
    w->edges[*n] = (cfw_edge){x0, y0, x1, y1};
    ++*n;
    return 1;
}

/* De Casteljau subdivision, <= 8 levels and 0.5 source pixel tolerance.
 * The compiler rejects edge overflow rather than silently dropping geometry. */
static int cv_curve(cfw_vector_work *w, uint32_t *n, const int32_t *p, uint32_t depth) {
    int32_t ux = 3*p[2]-2*p[0]-p[6], uy = 3*p[3]-2*p[1]-p[7];
    int32_t vx = 3*p[4]-2*p[6]-p[0], vy = 3*p[5]-2*p[7]-p[1];
    if (cv_abs(ux) <= 16 && cv_abs(uy) <= 16 && cv_abs(vx) <= 16 && cv_abs(vy) <= 16)
        return cv_edge(w,n,p[0],p[1],p[6],p[7]);
    if (depth == 8) return 0;
    int32_t a[8], b[8];
    for (uint32_t k=0;k<2;k++) {
        int32_t ab=(p[k]+p[k+2])/2, bc=(p[k+2]+p[k+4])/2, cd=(p[k+4]+p[k+6])/2;
        int32_t abc=(ab+bc)/2, bcd=(bc+cd)/2, mid=(abc+bcd)/2;
        a[k]=p[k]; a[k+2]=ab; a[k+4]=abc; a[k+6]=mid;
        b[k]=mid; b[k+2]=bcd; b[k+4]=cd; b[k+6]=p[k+6];
    }
    return cv_curve(w,n,a,depth+1) && cv_curve(w,n,b,depth+1);
}

/* Commands: 0 M(x,y), 1 L(x,y), 2 Q(cx,cy,x,y), 3 C(...), 4 Z.
 * Coordinates are signed int16 Q4; each subpath MUST explicitly close. */
static int cv_compile(cfw_vector_work *w, const uint8_t *src, uint32_t len) {
    uint32_t pos=0,n=0,commands=0;
    int open=0;
    int32_t x=0,y=0,sx=0,sy=0;
    if (!len || len > CFW_PATH_MAX_BYTES) return -1;
    while (pos<len) {
        uint8_t op=src[pos++];
        if (++commands > CFW_PATH_MAX_COMMANDS || op>4) return -1;
        uint32_t pairs=op==4?0:(op==2?2:(op==3?3:1));
        if (len-pos < pairs*4) return -1;
        int32_t p[8]={x,y,0,0,0,0,0,0};
        for (uint32_t k=0;k<pairs*2;k++) p[k+2]=(int16_t)rd16(src+pos+2*k);
        pos+=pairs*4;
        if (op==0) {
            if (open) return -1;
            x=sx=p[2]; y=sy=p[3]; open=1;
        } else {
            if (!open) return -1;
            if (op==4) {
                if (!cv_edge(w,&n,x,y,sx,sy)) return -1;
                x=sx; y=sy; open=0;
            } else if (op==1) {
                if (!cv_edge(w,&n,x,y,p[2],p[3])) return -1;
                x=p[2];y=p[3];
            } else {
                if (op==2) {
                    p[6]=p[4];p[7]=p[5];
                    for(uint32_t k=0;k<2;k++) {
                        p[k+4]=(p[k+6]+2*p[k+2])/3;
                        p[k+2]=(p[k]+2*p[k+2])/3;
                    }
                }
                if (!cv_curve(w,&n,p,0)) return -1;
                x=p[6];y=p[7];
            }
        }
    }
    return open ? -1 : (int)n;
}

static int32_t cv_sin(int32_t a) {
    a %= 360*256;
    if (a<0) a+=360*256;
    int32_t deg=a/256, f=a%256;
    /* Exact cardinal angles avoid a 32767/32768 shrink at 90 degrees. */
    int32_t v=deg==90?32768:(deg==270?-32768:cfw_sin_deg(deg));
    int32_t next=deg+1==90?32768:(deg+1==270?-32768:cfw_sin_deg(deg+1));
    return v+((next-v)*f)/256;
}

static void cv_rotate(int32_t *x, int32_t *y, int32_t sn,int32_t cs,int32_t px,int32_t py) {
    int32_t dx=*x-px,dy=*y-py;
    *x=px+(int32_t)(((int64_t)dx*cs-(int64_t)dy*sn+16384)>>15);
    *y=py+(int32_t)(((int64_t)dx*sn+(int64_t)dy*cs+16384)>>15);
}

static void cv_transform(cfw_vector_work *w, uint32_t n, const cfw_rotation *t) {
    if(t->angle%(360*256)==0)return;
    int32_t sn=cv_sin(t->angle),cs=cv_sin(t->angle%(360*256)+90*256);
    int32_t px=(int32_t)t->px*16,py=(int32_t)t->py*16;
    for(uint32_t i=0;i<n;i++) {
        cv_rotate(&w->edges[i].x0,&w->edges[i].y0,sn,cs,px,py);
        cv_rotate(&w->edges[i].x1,&w->edges[i].y1,sn,cs,px,py);
    }
}

static void cv_swap(cfw_vector_work *w,uint32_t a,uint32_t b) {
    int32_t x=w->xs[a];w->xs[a]=w->xs[b];w->xs[b]=x;
    int8_t wind=w->winds[a];w->winds[a]=w->winds[b];w->winds[b]=wind;
}
static void cv_sift(cfw_vector_work *w,uint32_t root,uint32_t count) {
    while(root*2+1<count) {
        uint32_t child=root*2+1;
        if(child+1<count && w->xs[child+1]>w->xs[child])child++;
        if(w->xs[root]>=w->xs[child])break;
        cv_swap(w,root,child);root=child;
    }
}
/* O(k log k), including adversarial contours with hundreds of crossings. */
static void cv_sort(cfw_vector_work *w,uint32_t count) {
    for(uint32_t i=count/2;i>0;i--)cv_sift(w,i-1,count);
    for(uint32_t i=count;i>1;i--){cv_swap(w,0,i-1);cv_sift(w,0,i-1);}
}

static void cv_fill(const cfw_raster *r, cfw_vector_work *w, uint32_t n, uint8_t rule, uint8_t color) {
    int32_t ymin=r->h*16, ymax=0;
    for(uint32_t i=0;i<n;i++) {
        cfw_edge e=w->edges[i];
        if(e.y0<ymin)ymin=e.y0;if(e.y1<ymin)ymin=e.y1;
        if(e.y0>ymax)ymax=e.y0;if(e.y1>ymax)ymax=e.y1;
    }
    int32_t first=cfw_clamp(cfw_div_floor(ymin,16),0,r->h);
    int32_t last=cfw_clamp(cfw_div_floor(ymax,16),-1,r->h-1);
    for(int32_t row=first;row<=last;row++) {
        int32_t yc=row*16+8; uint32_t k=0;
        for(uint32_t i=0;i<n;i++) {
            cfw_edge e=w->edges[i]; int8_t winding=1;
            if(e.y0==e.y1)continue;
            if(e.y0>e.y1) {int32_t v=e.x0;e.x0=e.x1;e.x1=v;v=e.y0;e.y0=e.y1;e.y1=v;winding=-1;}
            if(yc<e.y0 || yc>=e.y1)continue;
            int32_t x=e.x0+cv_muldiv(yc-e.y0,e.x1-e.x0,e.y1-e.y0);
            w->xs[k]=x;w->winds[k++]=winding;
        }
        cv_sort(w,k);
        int32_t winding=0,start=0;
        for(uint32_t i=0;i<k;) {
            int32_t x=w->xs[i], delta=0; uint32_t hits=0;
            do {delta+=w->winds[i++];hits++;} while(i<k && w->xs[i]==x);
            int before=rule?(winding&1):(winding!=0);
            winding=rule?(winding^(hits&1)):(winding+delta);
            int after=rule?(winding&1):(winding!=0);
            if(!before && after)start=x;
            if(before && !after) {int32_t a,b;if(rs_span_pixels(start,x,&a,&b))rs_hspan(r,a,b,row,color&15);}
        }
    }
}

static void cv_path_draw(const cfw_raster *r, cfw_vector_work *w, const cfw_path *path,
                         const int16_t *p, const cfw_rotation *t, uint8_t color) {
    int32_t scale=cfw_clamp(p[2],0,2048); /* Q8, 0..8x */
    for(uint32_t i=0;i<path->count;i++) {
        cfw_edge e=path->edges[i];
        e.x0=e.x0*scale/256+(int32_t)p[0]*16;e.y0=e.y0*scale/256+(int32_t)p[1]*16;
        e.x1=e.x1*scale/256+(int32_t)p[0]*16;e.y1=e.y1*scale/256+(int32_t)p[1]*16;
        w->edges[i]=e;
    }
    cv_transform(w,path->count,t);
    cv_fill(r,w,path->count,path->rule,color);
}

/* Rotated primitives become contours. Zero/full-turn angles keep the legacy
 * renderer exactly. Rounded outlines use outer/inner even-odd contours. */
static void cv_ellipse(cfw_vector_work *w,uint32_t *n,int32_t cx,int32_t cy,int32_t rad,
                       int32_t a0,int32_t span,int pie) {
    int32_t px=cx+(int32_t)(((int64_t)rad*cv_sin((a0%360+90)*256))>>15);
    int32_t py=cy+(int32_t)(((int64_t)rad*cv_sin((a0%360)*256))>>15);
    int32_t sx=px,sy=py;
    if(pie)cv_edge(w,n,cx,cy,px,py);
    uint32_t seg=(uint32_t)cv_abs(span)/4+1;
    for(uint32_t i=1;i<=seg;i++) {
        int32_t a=(a0*256+span*256*(int32_t)i/(int32_t)seg)%(360*256);
        int32_t nx=cx+(int32_t)(((int64_t)rad*cv_sin(a+90*256))>>15);
        int32_t ny=cy+(int32_t)(((int64_t)rad*cv_sin(a))>>15);
        cv_edge(w,n,px,py,nx,ny);px=nx;py=ny;
    }
    if(pie)cv_edge(w,n,px,py,cx,cy);
    else if(cv_abs(span)==360)cv_edge(w,n,px,py,sx,sy);
}

static void cv_roundrect(cfw_vector_work *w,uint32_t *n,int32_t x,int32_t y,int32_t ww,int32_t hh,int32_t rad) {
    if(ww<=0 || hh<=0)return;
    rad=cfw_clamp(rad,0,(ww<hh?ww:hh)/2);
    int32_t cx[4]={x+ww-rad,x+ww-rad,x+rad,x+rad};
    int32_t cy[4]={y+rad,y+hh-rad,y+hh-rad,y+rad};
    int32_t px=x+ww-rad,py=y,sx=px,sy=py;
    for(int k=0;k<4;k++)for(int j=0;j<=15;j++) {
        int32_t a=(-90+k*90+j*6)*256;
        int32_t nx=cx[k]+(int32_t)(((int64_t)rad*cv_sin(a+90*256))>>15);
        int32_t ny=cy[k]+(int32_t)(((int64_t)rad*cv_sin(a))>>15);
        cv_edge(w,n,px,py,nx,ny);px=nx;py=ny;
    }
    cv_edge(w,n,px,py,sx,sy);
}

static void cv_shape_draw(const cfw_raster *r,cfw_vector_work *w,const cfw_shape *s,
                           const cfw_rotation *t,cfw_rectlist *rl) {
    if(t->angle%(360*256)==0 || s->type>CFW_SHAPE_PIE) {cfw_shape_draw(r,s,rl);return;}
    uint32_t n=0;int fill=0;int32_t p[8];
    for(uint32_t i=0;i<8;i++)p[i]=(int32_t)s->p[i]*16+8;
    int32_t wd=s->width?s->width:1;
    switch(s->type) {
    case CFW_SHAPE_RECT: case CFW_SHAPE_RECT_FILL: {
        int32_t x=(int32_t)s->p[0]*16,y=(int32_t)s->p[1]*16;
        int32_t ww=cfw_clamp(s->p[2],0,2047)*16,hh=cfw_clamp(s->p[3],0,2047)*16;
        int32_t rad=cfw_clamp(s->p[4],0,2047)*16;
        cv_roundrect(w,&n,x,y,ww,hh,rad);
        if(s->type==CFW_SHAPE_RECT)cv_roundrect(w,&n,x+wd*16,y+wd*16,ww-wd*32,hh-wd*32,rad-wd*16);
        fill=1;break;
    }
    case CFW_SHAPE_CIRCLE:case CFW_SHAPE_CIRCLE_FILL: {
        int32_t rad=cfw_clamp(s->p[2],0,1023)*16+8;
        cv_ellipse(w,&n,p[0],p[1],rad,0,360,0);
        if(s->type==CFW_SHAPE_CIRCLE && rad>wd*16)cv_ellipse(w,&n,p[0],p[1],rad-wd*16,0,360,0);
        fill=1;break;
    }
    case CFW_SHAPE_ARC:case CFW_SHAPE_PIE: {
        int32_t span=cfw_clamp((int32_t)s->p[4]-s->p[3],-360,360);
        cv_ellipse(w,&n,p[0],p[1],cfw_clamp(s->p[2],0,1023)*16,s->p[3]%360,span,s->type==CFW_SHAPE_PIE);
        fill=s->type==CFW_SHAPE_PIE;break;
    }
    case CFW_SHAPE_BEZIER2:
        p[6]=p[4];p[7]=p[5];
        for(uint32_t k=0;k<2;k++){p[k+4]=(p[k+6]+2*p[k+2])/3;p[k+2]=(p[k]+2*p[k+2])/3;}
        /* fall through */
    case CFW_SHAPE_BEZIER3:
        /* Rendering remains bounded even for extreme legacy coordinates. */
        if(!cv_curve(w,&n,p,0))return;
        break;
    default: {
        uint32_t vertices=s->type==CFW_SHAPE_LINE?2:((s->type==CFW_SHAPE_TRI || s->type==CFW_SHAPE_TRI_FILL)?3:4);
        for(uint32_t i=1;i<vertices;i++)cv_edge(w,&n,p[2*i-2],p[2*i-1],p[2*i],p[2*i+1]);
        if(vertices>2)cv_edge(w,&n,p[2*vertices-2],p[2*vertices-1],p[0],p[1]);
        fill=s->type==CFW_SHAPE_TRI_FILL || s->type==CFW_SHAPE_QUAD_FILL;
    }}
    cv_transform(w,n,t);
    if(fill)cv_fill(r,w,n,1,s->color);
    else for(uint32_t i=0;i<n;i++) {
        cfw_edge e=w->edges[i];
        /* rs_line_q4's wide-line products require bounded deltas. Clip to
         * the panel plus stroke margin before entering the old rasterizer. */
        int32_t lo=-wd*16-16,hiX=(r->w+wd+1)*16,hiY=(r->h+wd+1)*16;
        for(int side=0;side<4;side++) {
            int axis=side/2;int32_t bound=(side&1)?(axis?hiY:hiX):lo;
            int32_t a=axis?e.y0:e.x0,b=axis?e.y1:e.x1;
            int outA=(side&1)?a>bound:a<bound,outB=(side&1)?b>bound:b<bound;
            if(outA && outB){e.x0=e.x1=0;e.y0=e.y1=-1000000;break;}
            if(outA!=outB) {
                int32_t den=b-a,num=bound-a,other;
                int32_t diff=axis?e.x1-e.x0:e.y1-e.y0;
                if(den<0){den=-den;num=-num;}
                other=(axis?e.x0:e.y0)+cv_muldiv(num,diff,den);
                if(outA){if(axis){e.y0=bound;e.x0=other;}else{e.x0=bound;e.y0=other;}}
                else{if(axis){e.y1=bound;e.x1=other;}else{e.x1=bound;e.y1=other;}}
            }
        }
        if(e.y0==-1000000)continue;
        rs_line_q4(r,e.x0,e.y0,e.x1,e.y1,wd,s->color&15);
    }
}
