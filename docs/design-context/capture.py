"""Record the actual native UI through SDL, without desktop recording permission.
Requires the existing CMake build, Swift/AVFoundation, Pillow, and a Retina display.
No product UI source is edited. Every scenario has an isolated disposable save.
"""
import json, os, shlex, shutil, subprocess, tempfile
from pathlib import Path
from PIL import Image, ImageOps, ImageDraw, ImageFont
ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent
BUILD = ROOT / 'emulator/build'
SWIFTC = shutil.which('swiftc')
assert SWIFTC, 'Swift compiler is required'
manifest=[]
with tempfile.TemporaryDirectory(prefix='gemmi-context-') as temp_name:
    temp=Path(temp_name)
    subprocess.run([SWIFTC,'-O',str(OUT/'encode.swift'),'-o',str(temp/'encoder')],check=True)
    source=(ROOT/'emulator/emulator.c').read_text()
    helper=(OUT/'capture_driver.inc').read_text()
    source=source.replace('int main(int argc, char **argv)',helper+'\nint main(int argc, char **argv)')
    source=source.replace('    uint32_t started = SDL_GetTicks();','    context_init(display);\n    uint32_t started = SDL_GetTicks();')
    source=source.replace('        uint32_t elapsed = SDL_GetTicks() - started;', '''        uint32_t elapsed = SDL_GetTicks() - started;
        if (context_tick(display, elapsed)) {
            int result = context_finish(); lv_sdl_quit(); return result;
        }''')
    (temp/'recorder.c').write_text(source)
    flags={}
    for line in (BUILD/'CMakeFiles/gemmi-emulator.dir/flags.make').read_text().splitlines():
        if ' = ' in line:
            k,v=line.split(' = ',1); flags[k]=shlex.split(v)
    subprocess.run(['/usr/bin/cc',*flags['C_DEFINES'],*flags['C_INCLUDES'],*flags['C_FLAGS'],'-c',str(temp/'recorder.c'),'-o',str(temp/'recorder.o')],check=True)
    link=shlex.split((BUILD/'CMakeFiles/gemmi-emulator.dir/link.txt').read_text())
    link=[str(temp/'recorder.o') if x=='CMakeFiles/gemmi-emulator.dir/emulator.c.o' else x for x in link]
    link[link.index('-o')+1]=str(temp/'recorder')
    subprocess.run(link,cwd=BUILD,check=True)
    def shot(ms,name): return (ms,'shot',0,0,name)
    def key(ms,k): return (ms,'key',0,0,k)
    def tap(ms,x,y,hold=100): return [(ms,'down',x,y,'-'),(ms+hold,'up',x,y,'-')]
    def record(name,scene,seconds,actions,gems=None):
        script=[]; shots=[]
        for a in sorted(actions):
            ms,kind,x,y,value=a
            if kind=='shot':
                shots.append(value)
                value=str(temp/(value+'.bmp'))
            script.append(f'{ms} {kind} {x} {y} {value}')
        print('Recording '+name,flush=True)
        script_path=temp/(name+'.txt'); script_path.write_text('\n'.join(script)+'\n')
        fifo=temp/(name+'.fifo'); os.mkfifo(fifo)
        video=OUT/'videos'/(name+'.mp4')
        log_path=Path('/tmp')/('gemmi-'+name+'.encoder.log')
        with log_path.open('w') as log:
            encoder=subprocess.Popen([str(temp/'encoder'),str(fifo),str(video)],stdout=log,stderr=log)
            args=[str(temp/'recorder'),'--scene',scene]
            if gems is not None: args+=['--test-gems',str(gems)]
            env=dict(os.environ,GEMMI_PET_SAVE=str(temp/(name+'.save')),GEMMI_CONTEXT_PIPE=str(fifo),GEMMI_CONTEXT_SCRIPT=str(script_path),GEMMI_CONTEXT_DURATION=str(int(seconds*1000)))
            try:
                result=subprocess.run(args,cwd=ROOT,env=env,capture_output=True,text=True,timeout=seconds+30)
                assert result.returncode==0,(name,result.stderr)
                assert encoder.wait(timeout=30)==0,log_path.read_text()
            finally:
                if encoder.poll() is None: encoder.kill(); encoder.wait()
        for s in shots:
            Image.open(temp/(s+'.bmp')).save(OUT/'screenshots'/(s+'.png'))
        manifest.append(dict(video='videos/'+name+'.mp4',scene=scene,duration=seconds,test_gems=gems,screenshots=['screenshots/'+s+'.png' for s in shots],events=actions,answer_output=result.stdout.strip()))
        print(name+f': {len(shots)} screenshots + video',flush=True)
    record('01-fresh-pet','home',7,[shot(400,'01-fresh-home'),*tap(800,120,105),shot(1500,'02-pet-reaction'),*tap(3200,120,105,1000),shot(4500,'03-hold-reaction')])
    record('02-task-activity','home',8,[key(500,'1'),shot(1500,'04-thinking'),key(3000,'2'),shot(4000,'05-working'),key(5500,'6'),shot(6500,'06-task-reward')])
    record('03-multiple-agents','agents',8,[*tap(500,40,299),shot(1200,'07-agent-list'),*tap(1700,90,98),shot(2400,'08-codex-log'),*tap(3100,120,299),shot(3800,'09-reviewer-log'),*tap(4700,40,299),shot(5400,'10-back-to-agents'),*tap(6100,40,299),shot(6800,'11-back-home')])
    record('04-two-choice-question','ask',7,[shot(400,'12-question-two-choices'),*tap(1100,40,299),shot(1600,'13-choice-selected'),*tap(2300,40,299),shot(2900,'14-answer-sent'),*tap(3800,120,299),shot(4400,'15-empty-asks'),*tap(5300,40,299)])
    record('05-question-back-and-reopen','ask-three',7,[shot(400,'16-question-three-choices'),*tap(1000,30,37),shot(1500,'17-question-pending-home'),*tap(2200,120,299),shot(2800,'18-question-reopened'),*tap(3500,195,299),shot(4000,'19-third-choice-selected'),*tap(4700,195,299),shot(5300,'20-third-choice-sent')])
    record('06-artifact-and-view','home',7,[*tap(500,195,299),shot(1100,'21-empty-view'),*tap(1800,40,299),key(2800,'4'),shot(3500,'22-artifact-reference'),*tap(4100,195,299),shot(4700,'23-artifact-view-with-back'),*tap(5500,40,299)])
    record('07-starter-shop','home',7,[*tap(600,222,12),shot(1300,'24-starter-shop'),*tap(1900,92,220),shot(2500,'25-not-enough-gems'),*tap(3200,40,220),shot(3800,'26-glasses-owned'),*tap(4600,214,181),shot(5300,'27-glasses-on-main')])
    record('08-shop-themes-and-bow','pet',9,[shot(500,'28-funded-shop-first-shelf'),*tap(1200,188,181),shot(1800,'29-shop-second-shelf'),*tap(2400,40,220),shot(2900,'30-rose-equipped'),*tap(3500,92,220),shot(4000,'31-sky-equipped'),*tap(4600,144,220),shot(5100,'32-dusk-equipped'),*tap(5700,196,220),shot(6300,'33-bow-equipped'),*tap(7000,214,181),shot(7700,'34-bow-on-main')],gems=1000)
    record('09-crown-and-remove','pet',6,[*tap(500,92,220),shot(1200,'35-crown-equipped'),*tap(2000,214,181),shot(2600,'36-crown-on-main'),*tap(3400,222,12),*tap(4100,92,220),shot(4800,'37-owned-crown-removed')],gems=1000)
    record('10-level-up','level-up',5,[shot(800,'38-level-up-jump'),shot(2700,'39-level-up-settled')])
    record('11-connectivity','offline',5,[shot(700,'40-disconnected'),key(2300,'i'),shot(3200,'41-reconnected')])
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
# A compact, labeled overview for quick agent orientation.
files=sorted((OUT/'screenshots').glob('*.png'))
thumb_w,thumb_h=192,256
sheet=Image.new('RGB',(5*224,((len(files)+4)//5)*294),'#16141b')
draw=ImageDraw.Draw(sheet)
for i,p in enumerate(files):
    x=(i%5)*224+16; y=(i//5)*294+8
    with Image.open(p) as im: sheet.paste(im.resize((thumb_w,thumb_h),Image.Resampling.LANCZOS),(x,y))
    draw.text((x,y+261),p.stem[:29],fill='#e4dbea')
sheet.save(OUT/'contact-sheet.jpg',quality=90)
print('Done: '+str(OUT),flush=True)

import html
video_cards=[]
for entry in manifest:
    title=entry['video'].split('/')[-1].removesuffix('.mp4').replace('-', ' ')
    poster=entry['screenshots'][0]
    video_cards.append(f'<article><h3>{html.escape(title)}</h3><video controls preload="none" poster="{poster}" src="{entry["video"]}"></video><a href="{entry["video"]}">Open MP4</a></article>')
image_cards=[]
for file in files:
    title=file.stem.replace('-', ' ')
    image_cards.append(f'<a class="shot" href="screenshots/{file.name}"><img loading="lazy" src="screenshots/{file.name}" alt="{html.escape(title)}"><span>{html.escape(title)}</span></a>')
(OUT/'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Gemmi — native design reference</title><style>body{margin:32px;background:#121016;color:#ece8f1;font:14px/1.5 ui-monospace,Menlo,monospace}a{color:#c7addc}h1{font-size:24px}h2{margin-top:40px}h3{font-size:13px;font-weight:500}nav{display:flex;gap:24px;flex-wrap:wrap}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:24px}article, .shot{min-width:0}video{width:100%;max-width:300px;aspect-ratio:3/4;background:white;display:block;margin-bottom:8px}.shot img{width:100%;display:block}.shot span{display:block;font-size:12px;margin-top:8px}.shot{text-decoration:none}p{max-width:850px;color:#b9afc4}</style><h1>Gemmi / Native design reference</h1><p>Actual C/LVGL emulator recordings. Fresh local pet reset; each recording uses an isolated fixture save. Shop test funding is explicit in the guide. 2026-10-17 · source 8ef1857.</p><nav><a href="README.md">Agent guide</a><a href="contact-sheet.jpg">Contact sheet</a><a href="manifest.json">Scenario manifest</a><a href="#videos">Videos</a><a href="#screenshots">Screenshots</a></nav><h2 id="videos">Native recordings</h2><div class="grid">' + ''.join(video_cards) + '</div><h2 id="screenshots">Screenshots</h2><div class="grid">' + ''.join(image_cards) + '</div></html>')
