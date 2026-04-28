"""
FlyteOS 飞行模拟 - 静态报告生成（无GUI模式）
生成完整飞行仿真数据并输出PNG报告图
"""
import math, time, random
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
from matplotlib.patches import FancyArrow
from collections import deque
from dataclasses import dataclass, field
from typing import List

# ═══════════ 物理常数 ═══════════
G=9.81; RHO_AIR=1.225; RHO_HE=0.1786; DT=0.02

@dataclass
class Vec3:
    x:float=0.0; y:float=0.0; z:float=0.0
    def __add__(self,o): return Vec3(self.x+o.x,self.y+o.y,self.z+o.z)
    def __sub__(self,o): return Vec3(self.x-o.x,self.y-o.y,self.z-o.z)
    def __mul__(self,s): return Vec3(self.x*s,self.y*s,self.z*s)
    def norm(self): return math.sqrt(self.x**2+self.y**2+self.z**2)

@dataclass
class State:
    pos:Vec3=field(default_factory=Vec3)
    vel:Vec3=field(default_factory=Vec3)
    roll:float=0.0; pitch:float=0.0; yaw:float=0.0
    p:float=0.0;    q:float=0.0;     r:float=0.0
    solar_w:float=0.0; turbine_w:float=0.0; bat_pct:float=100.0
    buoy_ratio:float=0.60
    motors:List[float]=field(default_factory=lambda:[0.0]*4)
    time_s:float=0.0

class PID:
    def __init__(self,kp,ki,kd,il=10,ol=1):
        self.kp,self.ki,self.kd,self.il,self.ol=kp,ki,kd,il,ol
        self.integral=self.prev=0.0
    def update(self,e,dt):
        self.integral=np.clip(self.integral+e*dt,-self.il,self.il)
        d=(e-self.prev)/max(dt,1e-6); self.prev=e
        return float(np.clip(self.kp*e+self.ki*self.integral+self.kd*d,-self.ol,self.ol))
    def reset(self): self.integral=self.prev=0.0

class Physics:
    def __init__(self):
        self.mass=12.0; self.vol=2.5; self.max_T=60.0
        self.drag=0.4; self.I=Vec3(0.3,0.3,0.5)
    def buoy(self,alt): return (RHO_AIR*math.exp(-alt/8500)-RHO_HE)*self.vol*G
    def step(self,s:State,motors:List[float])->State:
        dt=DT; m0,m1,m2,m3=[np.clip(m,0,1) for m in motors]
        total=(m0+m1+m2+m3)/4; T=total*self.max_T
        bf=self.buoy(-s.pos.z)
        rt=(m0-m1-m2+m3)*self.max_T*0.15
        pt=(-m0-m1+m2+m3)*self.max_T*0.15
        yt=(m0-m1+m2-m3)*self.max_T*0.02
        s.p+=((rt/self.I.x)-s.q*s.r*(self.I.z-self.I.y)/self.I.x)*dt; s.p*=0.97
        s.q+=((pt/self.I.y)-s.p*s.r*(self.I.x-self.I.z)/self.I.y)*dt; s.q*=0.97
        s.r+=(yt/self.I.z)*dt; s.r*=0.97
        s.roll+=s.p*dt; s.pitch+=s.q*dt; s.yaw+=s.r*dt
        s.roll=np.clip(s.roll,-0.4,0.4); s.pitch=np.clip(s.pitch,-0.4,0.4)
        cp,sp=math.cos(s.pitch),math.sin(s.pitch)
        cr,sr=math.cos(s.roll),math.sin(s.roll)
        cy,sy=math.cos(s.yaw),math.sin(s.yaw)
        fn=T*(-sp*cy); fe=T*(-sp*sy); fd=-(T*cp*cr+bf)
        spd=s.vel.norm(); dg=self.drag*spd**2*0.1/max(spd,0.01)
        ax=(fn-s.vel.x*dg)/self.mass
        ay=(fe-s.vel.y*dg)/self.mass
        az=(fd+self.mass*G-s.vel.z*dg)/self.mass
        s.vel.x+=ax*dt; s.vel.y+=ay*dt; s.vel.z+=az*dt
        s.vel.x=np.clip(s.vel.x,-15,15); s.vel.y=np.clip(s.vel.y,-15,15); s.vel.z=np.clip(s.vel.z,-8,8)
        s.pos.x+=s.vel.x*dt; s.pos.y+=s.vel.y*dt; s.pos.z+=s.vel.z*dt
        if s.pos.z>0: s.pos.z=0.0; s.vel.z=min(s.vel.z,0)
        irr=800*max(0,math.cos(s.time_s*0.0002))
        s.solar_w=2.5*0.22*irr; s.turbine_w=total*80
        s.bat_pct=np.clip(s.bat_pct+(s.solar_w+s.turbine_w-total*150)*dt/200,0,100)
        s.buoy_ratio=bf/(self.mass*G+1e-3)
        s.motors=motors[:]; s.time_s+=dt; return s

class Controller:
    def __init__(self):
        self.pid_n=PID(1.5,0.02,0.3,5,6); self.pid_e=PID(1.5,0.02,0.3,5,6)
        self.pid_d=PID(2.0,0.05,0.4,3,3)
        self.pid_vn=PID(2.5,0.05,0.2,8,0.35); self.pid_ve=PID(2.5,0.05,0.2,8,0.35)
        self.pid_vd=PID(3.0,0.10,0.3,5,0.8)
        self.pid_r=PID(5.0,0.1,0.4,3,0.5); self.pid_p=PID(5.0,0.1,0.4,3,0.5)
        self.pid_y=PID(3.0,0.05,0.2,2,0.4)
        self.tp=Vec3(); self.ty=0.0; self.armed=False
    def set_target(self,p,y=0): self.tp=p; self.ty=y
    def compute(self,s:State)->List[float]:
        if not self.armed: return [0.0]*4
        dt=DT
        vn=self.pid_n.update(self.tp.x-s.pos.x,dt)
        ve=self.pid_e.update(self.tp.y-s.pos.y,dt)
        vd=self.pid_d.update(self.tp.z-s.pos.z,dt)
        cy,sy=math.cos(s.yaw),math.sin(s.yaw)
        an=self.pid_vn.update(vn-s.vel.x,dt); ae=self.pid_ve.update(ve-s.vel.y,dt)
        rc=np.clip(ae*cy-an*sy,-0.35,0.35); pc=np.clip(-(an*cy+ae*sy),-0.35,0.35)
        ye=self.ty-s.yaw
        while ye>math.pi: ye-=2*math.pi
        while ye<-math.pi: ye+=2*math.pi
        th=np.clip(0.45+self.pid_vd.update(vd-s.vel.z,dt),0.1,0.9)
        th*=(1-s.buoy_ratio*0.6); th=np.clip(th,0.05,0.9)
        ro=self.pid_r.update(rc-s.roll,dt); po=self.pid_p.update(pc-s.pitch,dt)
        yo=self.pid_y.update(ye,dt)
        return [np.clip(x,0,1) for x in [th+ro-po+yo,th-ro-po-yo,th-ro+po+yo,th+ro+po-yo]]

MISSION=[
    (0,0,-5,2,"起飞"),
    (0,0,-35,3,"上升至35m"),
    (40,0,-40,3,"WP1"),
    (40,40,-50,3,"WP2"),
    (0,40,-40,3,"WP3"),
    (0,0,-40,3,"WP4返回"),
    (0,0,-10,2,"下降"),
    (0,0,0,1,"着陆"),
]

def run_simulation():
    phys=Physics(); ctrl=Controller(); s=State()
    ctrl.armed=True
    records=dict(t=[],alt=[],vn=[],ve=[],vd=[],roll=[],pitch=[],yaw=[],
                 solar=[],bat=[],buoy=[],px=[],py=[],motors0=[],motors1=[],motors2=[],motors3=[])
    wp_idx=0; hover_t=0.0; wp_events=[]
    for _ in range(18000):   # 最多360s
        if wp_idx<len(MISSION):
            n,e,az,hs,lb=MISSION[wp_idx]
            ctrl.set_target(Vec3(n,e,az))
            d=math.sqrt((s.pos.x-n)**2+(s.pos.y-e)**2+(s.pos.z-az)**2)
            if d<3.0:
                hover_t+=DT
                if hover_t>=hs:
                    hover_t=0.0
                    wp_events.append((s.time_s,lb))
                    wp_idx+=1
        else:
            ctrl.armed=False
        motors=ctrl.compute(s)
        s=phys.step(s,motors)
        records['t'].append(s.time_s)
        records['alt'].append(-s.pos.z)
        records['vn'].append(s.vel.x); records['ve'].append(s.vel.y); records['vd'].append(-s.vel.z)
        records['roll'].append(math.degrees(s.roll)); records['pitch'].append(math.degrees(s.pitch))
        records['yaw'].append(math.degrees(s.yaw))
        records['solar'].append(s.solar_w); records['bat'].append(s.bat_pct)
        records['buoy'].append(s.buoy_ratio*100)
        records['px'].append(s.pos.x); records['py'].append(s.pos.y)
        records['motors0'].append(motors[0]); records['motors1'].append(motors[1])
        records['motors2'].append(motors[2]); records['motors3'].append(motors[3])
        if wp_idx>=len(MISSION) and -s.pos.z<0.5: break
    return records, wp_events

print("正在运行飞行仿真...")
records, wp_events = run_simulation()
t=records['t']
print(f"仿真完成，飞行时间 {t[-1]:.1f}s，共 {len(t)} 步")

# ═══════════ 绘图 ═══════════════════════════════════════════════════
plt.rcParams['font.sans-serif']=['SimHei','Microsoft YaHei','DejaVu Sans']
plt.rcParams['axes.unicode_minus']=False
BG='#0a0a1a'; PANEL='#0d1117'; GRID='#1f2937'; ACC='#00d4ff'

fig=plt.figure(figsize=(20,14),facecolor=BG)
fig.suptitle('FlyteOS 浮空飞行器飞行仿真报告  |  武汉福莱特航空科技有限公司',
             fontsize=16,color=ACC,fontweight='bold',y=0.99)

gs=GridSpec(3,4,figure=fig,hspace=0.55,wspace=0.4,
            left=0.07,right=0.97,top=0.94,bottom=0.07)

def styled(ax,title,xlabel='时间 (s)',ylabel=''):
    ax.set_facecolor(PANEL); ax.set_title(title,color=ACC,fontsize=10,pad=6)
    ax.set_xlabel(xlabel,color='#888',fontsize=8); ax.set_ylabel(ylabel,color='#888',fontsize=8)
    ax.tick_params(colors='#aaa',labelsize=8)
    for sp in ax.spines.values(): sp.set_color(GRID)
    ax.grid(True,color=GRID,alpha=0.6,linewidth=0.7)

# ── 1. 3D 轨迹俯视 ─────────────────────────────────────────────────
ax1=fig.add_subplot(gs[0:2,0:2])
styled(ax1,'飞行轨迹（俯视 NED）','East (m)','North (m)')
px,py=records['px'],records['py']
sc=ax1.scatter(py,px,c=records['alt'],cmap='plasma',s=1.5,alpha=0.8)
plt.colorbar(sc,ax=ax1,label='高度 (m)',shrink=0.8).ax.yaxis.set_tick_params(color='#aaa')
for _,lb in wp_events:
    i=min(range(len(t)),key=lambda j:abs(t[j]-_))
    ax1.annotate(lb,(py[i],px[i]),fontsize=7,color='#ffaa00',
                 arrowprops=dict(arrowstyle='->',color='#ffaa00',lw=0.8),
                 xytext=(py[i]+3,px[i]+3))
# 航点
for n,e,az,_,lb in MISSION:
    ax1.plot(e,n,'o',color='#ffaa00',ms=7,zorder=5)
ax1.plot(py[0],px[0],'g^',ms=10,label='起飞点',zorder=6)
ax1.plot(py[-1],px[-1],'rv',ms=10,label='降落点',zorder=6)
ax1.legend(fontsize=8,facecolor='#1f2937',labelcolor='white')

# ── 2. 高度曲线 ───────────────────────────────────────────────────
ax2=fig.add_subplot(gs[0,2])
styled(ax2,'高度曲线','时间 (s)','高度 (m)')
ax2.fill_between(t,records['alt'],alpha=0.3,color=ACC)
ax2.plot(t,records['alt'],color=ACC,lw=1.5)
ax2.set_ylim(bottom=0)
for wt,lb in wp_events:
    ax2.axvline(wt,color='#ffaa00',ls='--',lw=0.8,alpha=0.7)
    ax2.text(wt,max(records['alt'])*0.9,lb,fontsize=6.5,color='#ffaa00',rotation=90,va='top')

# ── 3. 三轴速度 ───────────────────────────────────────────────────
ax3=fig.add_subplot(gs[0,3])
styled(ax3,'速度曲线','时间 (s)','速度 (m/s)')
ax3.plot(t,records['vn'],color='#ff6b6b',lw=1.2,label='北向')
ax3.plot(t,records['ve'],color='#ffd93d',lw=1.2,label='东向')
ax3.plot(t,records['vd'],color='#00ff88',lw=1.2,label='垂向')
ax3.axhline(0,color=GRID,lw=0.7)
ax3.legend(fontsize=7,facecolor='#1f2937',labelcolor='white')

# ── 4. 姿态角 ─────────────────────────────────────────────────────
ax4=fig.add_subplot(gs[1,2])
styled(ax4,'姿态角','时间 (s)','角度 (°)')
ax4.plot(t,records['roll'], color='#ff6b6b',lw=1.2,label='横滚Roll')
ax4.plot(t,records['pitch'],color='#ffd93d',lw=1.2,label='俯仰Pitch')
ax4.fill_between(t,records['roll'],alpha=0.15,color='#ff6b6b')
ax4.fill_between(t,records['pitch'],alpha=0.15,color='#ffd93d')
ax4.axhline(0,color=GRID,lw=0.7)
ax4.legend(fontsize=7,facecolor='#1f2937',labelcolor='white')

# ── 5. 偏航角 ─────────────────────────────────────────────────────
ax5=fig.add_subplot(gs[1,3])
styled(ax5,'偏航角','时间 (s)','偏航 (°)')
ax5.plot(t,records['yaw'],color='#88ccff',lw=1.4)
ax5.fill_between(t,records['yaw'],alpha=0.2,color='#88ccff')

# ── 6. 氦气浮力比 ─────────────────────────────────────────────────
ax6=fig.add_subplot(gs[2,0])
styled(ax6,'氦气浮力占比','时间 (s)','浮力/重力 (%)')
ax6.fill_between(t,records['buoy'],alpha=0.4,color='#88ffcc')
ax6.plot(t,records['buoy'],color='#88ffcc',lw=1.4)
ax6.axhline(60,color='#ffaa00',ls='--',lw=1,label='设计目标60%')
ax6.set_ylim(0,120)
ax6.legend(fontsize=7,facecolor='#1f2937',labelcolor='white')

# ── 7. 太阳能功率 ─────────────────────────────────────────────────
ax7=fig.add_subplot(gs[2,1])
styled(ax7,'太阳能功率（MPPT）','时间 (s)','功率 (W)')
ax7.fill_between(t,records['solar'],alpha=0.4,color='#ffd93d')
ax7.plot(t,records['solar'],color='#ffd93d',lw=1.4)

# ── 8. 电池电量 ───────────────────────────────────────────────────
ax8=fig.add_subplot(gs[2,2])
styled(ax8,'电池电量','时间 (s)','电量 (%)')
bat=records['bat']
ax8.fill_between(t,bat,alpha=0.3,color='#00ff88')
ax8.plot(t,bat,color='#00ff88',lw=1.4)
ax8.set_ylim(0,105)
ax8.axhline(30,color='#ff6b35',ls='--',lw=1,label='低电量警戒线')
ax8.legend(fontsize=7,facecolor='#1f2937',labelcolor='white')

# ── 9. 电机PWM ────────────────────────────────────────────────────
ax9=fig.add_subplot(gs[2,3])
styled(ax9,'电机PWM输出（4旋翼混控）','时间 (s)','PWM (0~1)')
colors_m=['#ff6b6b','#ffd93d','#00ff88','#00d4ff']
labels_m=['M1前左','M2前右','M3后右','M4后左']
for i,col,lb in zip(range(4),colors_m,labels_m):
    ax9.plot(t,records[f'motors{i}'],color=col,lw=0.9,label=lb,alpha=0.85)
ax9.legend(fontsize=6.5,facecolor='#1f2937',labelcolor='white',ncol=2)
ax9.set_ylim(0,1.05)

# 统计信息
max_alt=max(records['alt']); total_dist=0
for i in range(1,len(records['px'])):
    dx=records['px'][i]-records['px'][i-1]; dy=records['py'][i]-records['py'][i-1]
    total_dist+=math.sqrt(dx*dx+dy*dy)
avg_solar=sum(records['solar'])/len(records['solar'])
min_bat=min(records['bat'])

info=f"飞行时间: {t[-1]:.0f}s  |  最大高度: {max_alt:.1f}m  |  飞行里程: {total_dist:.0f}m  |  平均太阳能: {avg_solar:.0f}W  |  最低电量: {min_bat:.1f}%"
fig.text(0.5,0.015,info,ha='center',fontsize=9,color='#aaaaaa',
         bbox=dict(boxstyle='round',facecolor='#1f2937',edgecolor=GRID,alpha=0.8))

out_path='E:/WorkBuddy/Claw/FlyteOS/飞行仿真报告.png'
plt.savefig(out_path,dpi=150,bbox_inches='tight',facecolor=BG)
print(f"报告已保存: {out_path}")
