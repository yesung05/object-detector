const fs=require('node:fs'),path=require('node:path'),{execFileSync}=require('node:child_process');
const dir=path.resolve(process.argv[2]),rows=JSON.parse(fs.readFileSync(path.join(dir,'audit.json'),'utf8')).results;
for(let offset=0;offset<rows.length;offset+=4){const group=rows.slice(offset,offset+4),args=['-hide_banner','-loglevel','error','-filter_complex_threads','1'];const cells=[];for(const r of group)for(const label of ['baseline','late_observe','end']){const f=r.review_frames.find(v=>v.label===label);args.push('-i',path.join(dir,f.file));cells.push({id:r.id,...f});}
 const filters=cells.map((c,i)=>`[${i}:v]crop=270:165:120:190,scale=540:330,drawtext=fontfile='C\\:/Windows/Fonts/arial.ttf':text='${c.id} ${c.label} ${c.requested_s.toFixed(1)}s':x=5:y=5:fontsize=19:fontcolor=red[v${i}]`);
 const layout=cells.map((_,i)=>`${i%3*540}_${Math.floor(i/3)*330}`).join('|');filters.push(cells.map((_,i)=>`[v${i}]`).join('')+`xstack=inputs=${cells.length}:layout=${layout}[out]`);
 args.push('-filter_complex',filters.join(';'),'-map','[out]','-frames:v','1','-n',path.join(dir,`sheet-${offset/4+1}.jpg`));execFileSync('C:/dev/ffmpeg-master-latest-win64-gpl-shared/bin/ffmpeg.exe',args,{windowsHide:true,timeout:30000,stdio:'pipe'});
}console.log('Sheets created');
