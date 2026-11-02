/* Firmware updates from the meatpi-devices manifests (2026-10-18,
   internal/TASK_fw_update.md): the Updates card of System > Firmware update
   over /api/fw_update (fw_update/HTTP_API.md). An on-demand chunk
   (/ui/fwupdate.js): the System page loads it into the Firmware pane ahead
   of the manual upload card.

   One row per product: the WiCAN PRO and, since 2026-10-19, the ESPNetLink
   dongle updated through the WiCAN (the relay): the installed version with
   its build, what the channel offers, Release notes and Update at the right
   edge, the progress of a job with Cancel, the outcome of the last job
   (verified, pending, rolled back; for the dongle: updated or why not), the
   wipe dialog for a release whose partition layout formats data, and the
   fw_update settings form (staged: the global Submit changes applies it).
   Downloads go over WiFi only: on the dongle's mobile connection the card
   lists the updates and says why the buttons wait. */
(()=>{
const JOB_TEXT={checking:"Checking the release list",downloading:"Downloading",verifying:"Verifying the image",
  rebooting:"Installed, restarting",pending_verify:"Installed, verifying the new firmware (10 minutes, or a planned restart)",
  verified:"Verified",rolled_back:"Rolled back",boot_failed:"The new firmware did not start",failed:"Failed",aborted:"Cancelled"};
const DONGLE_JOB_TEXT={checking:"Checking the dongle release list",downloading:"Downloading dongle firmware",verifying:"Verifying the dongle image",
  pushing:"Sending to the dongle",rebooting:"The dongle is restarting; internet through it pauses for about a minute",
  pending_verify:"Checking the dongle's new firmware",verified:"Dongle updated",failed:"Dongle update failed",aborted:"Cancelled"};
const REASON_TEXT={up_to_date:"Up to date",no_release:"The channel carries no release yet",unknown_version:"This build is not in the release list: update by hand",
  no_path:"An update exists, but it needs a stepping stone this channel does not carry",layout_serial:"Needs a USB flash from a computer",
  layout_unknown:"The release list does not know this device's partition layout",no_manifest:"Not checked yet"};
const DONGLE_VERDICT_TEXT={not_present:"No ESPNetLink dongle is connected",unreachable:"The dongle is not reachable right now",
  needs_usb_flash:"This dongle firmware cannot be updated from the WiCAN PRO. Update it once over USB from a computer; later releases install from here.",
  no_manifest:"Not checked yet",up_to_date:"Up to date",layout_mismatch:"The release list does not cover this dongle's partition layout: update it over USB",
  no_path:"An update exists, but it needs a stepping stone this channel does not carry",unknown_version:"This dongle build is not in the release list"};
const CODE_TEXT={busy:"another update job is running",busy_upload:"a firmware upload is in progress",no_network:"the device is not connected",
  no_manifest:"check for updates first",unknown_version:"no such release",not_visible:"that release is not on the selected channel",
  yanked:"that release was withdrawn",needs_confirm_wipe:"the new partition layout erases data",layout_serial:"needs a USB flash",
  layout_unknown:"the partition layout is unknown to the release list",cellular_blocked:"the device is online through the mobile connection: downloads need WiFi",
  insufficient_ram:"not enough free memory right now",sha256_mismatch:"the downloaded image did not match its digest",
  wrong_product:"the image is not for this device",wrong_image:"the download is not a firmware image",image_fetch_failed:"the download broke off",
  image_not_found:"the image is not where the release list says",size_mismatch:"the server offers a file of another size",
  manifest_fetch_failed:"the release list could not be fetched",flash_failed:"the flash write failed",image_invalid:"the image did not validate",
  no_slot:"no free firmware slot",image_too_large:"the image does not fit the firmware slot",
  no_dongle:"no ESPNetLink dongle is reachable",needs_usb_flash:"this dongle firmware cannot be updated from here: flash it once over USB",
  dongle_busy:"the dongle is receiving another image",dongle_rejected:"the dongle refused the image",push_failed:"the image could not be sent to the dongle",
  timeout_reboot:"the dongle did not come back after the update: unplug it and plug it in again",
  version_mismatch_after:"the dongle came back with another version (it may have restarted on its previous firmware after crashing)",
  no_memory:"not enough free memory to stage the dongle image",dongle_absent:"updating the dongle is not built into this firmware"};
const DOCS_URL="https://meatpihq.github.io/wican-fw/config/firmware-update";
const DONGLE_DOCS_URL="https://meatpihq.github.io/wican-fw/espnetlink/firmware-update";
const NOTES_REPO="https://github.com/meatpiHQ/meatpi-devices";
const ago=s=>s==null?"":s<90?s+" s ago":s<5400?Math.round(s/60)+" min ago":s<172800?Math.round(s/3600)+" h ago":Math.round(s/86400)+" d ago";
const mb=b=>(b/1048576).toFixed(1)+" MB";
const codeText=c=>CODE_TEXT[c]||c||"unknown error";

/* a small Markdown subset, every byte escaped: headings, lists, bold,
   inline code, https links in a new tab, paragraphs */
function mdLite(text){
  const esc=s=>s.replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
  const inline=s=>esc(s)
    .replace(/\*\*([^*]+)\*\*/g,"<b>$1</b>")
    .replace(/`([^`]+)`/g,"<code>$1</code>")
    .replace(/\[([^\]]+)\]\((https:\/\/[^)\s]+)\)/g,'<a href="$2" target="_blank" rel="noopener">$1</a>');
  const out=[];let list=null,para=[];
  const flush=()=>{if(para.length){out.push("<p>"+para.map(inline).join(" ")+"</p>");para=[];}if(list){out.push("<ul>"+list.join("")+"</ul>");list=null;}};
  for(const raw of String(text||"").split(/\r?\n/)){
    const ln=raw.trimEnd();
    if(!ln.trim()){flush();continue;}
    const hm=/^(#{1,3})\s+(.*)$/.exec(ln);
    if(hm){flush();out.push("<h"+(hm[1].length+2)+">"+inline(hm[2])+"</h"+(hm[1].length+2)+">");continue;}
    const lm=/^\s*[-*]\s+(.*)$/.exec(ln);
    if(lm){if(para.length){out.push("<p>"+para.map(inline).join(" ")+"</p>");para=[];}(list=list||[]).push("<li>"+inline(lm[1])+"</li>");continue;}
    if(list){out.push("<ul>"+list.join("")+"</ul>");list=null;}
    para.push(ln.trim());
  }
  flush();
  return out.join("");
}

/* the notes come straight from the release server into the browser (2026-11-02):
   the device only names their address (the snapshot's notes_url) and never
   downloads them; a phone without internet sees the message below while the
   device's own check and install carry on */
async function notesModal(url,version,target){
  let body;
  try{
    if(!url)throw new Error("no notes for this release");
    const r=await fetch(url,{cache:"no-cache"});
    const t=await r.text();
    if(!r.ok)throw new Error("HTTP "+r.status);
    body=h("div",{class:"fwu-notes",html:mdLite(t)});
  }catch(e){body=h("p",{},"The release notes could not be loaded in this browser (they come from the release server, so this phone or PC needs internet): "+e.message);}
  modal({title:"Release notes"+(version?" "+version:"")+(target==="dongle"?" (ESPNetLink)":""),body,
    actions:[{label:"Open on GitHub",fn:()=>window.open(url||NOTES_REPO,"_blank")},{label:"Close",kind:"pri"}]});
}

async function post(path,body){return api(path,{method:"POST",body:body||{}});}

/* the wipe dialog: what is erased, the backup first, then the install */
function wipeDialog(tg,st,go){
  const what=(tg.wipes||[]).map(w=>w==="storage"?"stored files, logs and the saved configuration":w==="settings"?"settings":w).join(", ");
  modal({title:"This update erases data",
    body:h("div",{},
      h("p",{},"Version "+tg.version+" uses a new partition layout. On its first start the device updates the layout and erases: ",h("b",{},what||"data partitions"),"."),
      h("ol",{style:"padding-left:18px;line-height:1.6"},
        h("li",{},"Download a backup first: ",h("a",{href:"#/system/maintenance",onclick:e=>{e.preventDefault();location.hash="#/system/maintenance";}},"System > Maintenance > Configuration backup"),"."),
        h("li",{},st.job&&st.job.backup&&st.job.backup.sd!==undefined?"":"With a card in, a sealed copy is written to the card before the download."),
        h("li",{},"Install. The device restarts twice; restore from the backup afterwards.")),
      h("p",{class:"help"},"Nothing is erased until you confirm here.")),
    actions:[{label:"Cancel"},{label:"Install and erase",kind:"danger",fn:go}]});
}

/* the dongle dialog: what happens to the dongle and the internet through it */
function dongleDialog(d,go){
  modal({title:"Update the ESPNetLink to "+d.target_version+"?",
    body:h("div",{},
      h("p",{},"The WiCAN PRO downloads the dongle firmware ("+(d.latest&&d.latest.size?mb(d.latest.size):"about 2 MB")+"), checks it, and sends it to the dongle over "+(d.transport==="usb"?"the USB cable":"its WiFi")+"."),
      h("p",{},"The dongle restarts once; internet through it pauses for about a minute. The WiCAN PRO itself keeps running."),
      h("p",{class:"help"},"Keep the dongle plugged in until the row says Dongle updated.")),
    actions:[{label:"Cancel"},{label:"Update the dongle",kind:"pri",fn:go}]});
}

async function card(){
  let st=await api("/api/fw_update");
  const rows=h("div",{class:"fwu-rows"});
  const prog=h("div",{class:"fwu-progress",style:"display:none;margin-top:12px"});
  const result=h("div",{class:"fwu-result"});
  const checked=h("span",{class:"help fwu-checked"});
  const checkBtn=h("button",{class:"btn sm fwu-check",onclick:async()=>{
    checkBtn.disabled=true;
    try{await post("/api/fw_update/check");toast("Checking for updates","ok");pollUntilIdle();}
    catch(e){toast(e.message,"err");checkBtn.disabled=false;}}},ic("refresh"),"Check now");
  const settingsRows=await settingsForm("fw_update",["enabled","channel","check_interval_h","base_url","cert_set","cli"],
    {visible:false,advanced:["base_url","cert_set"]});
  let timer=null;

  const install=async(target,opts)=>{
    const body={target};
    if(opts&&opts.version)body.version=opts.version;
    if(opts&&opts.confirm_wipe)body.confirm_wipe=true;
    try{
      const r=await post("/api/fw_update/install",body);
      if(r&&r.already){toast("Already running "+(r.installed||"that version"),"ok");return;}
      toast(target==="dongle"?"Dongle update started":"Update started","ok");pollUntilIdle();
    }catch(e){toast("Update refused: "+e.message,"err");}
  };
  const startInstall=(tg)=>{
    const go=(extra)=>install("self",Object.assign({version:tg.version},extra||{}));
    const needWipe=tg.verdict==="wipe";
    if(needWipe)wipeDialog(tg,st,()=>go({confirm_wipe:true}));else go();
  };
  const startDongle=(d)=>dongleDialog(d,()=>install("dongle",{version:d.target_version}));
  const mobile=()=>!!(st.cellular&&st.cellular.is_cellular);

  function wicanRow(){
    const self=st.targets.self,tg=self.target,job=st.job;
    const second=self.available&&tg
      ?h("div",{class:"help fwu-avail"},tg.version+" available"+(self.latest&&self.latest.date?", "+self.latest.date:"")+(self.latest&&self.latest.summary?", "+self.latest.summary:"")
        +(tg.step?" (first step: more updates follow)":""))
      :h("div",{class:"help fwu-reason"},REASON_TEXT[self.reason]||self.reason||"Up to date");
    const right=[];
    if(self.latest&&self.latest.notes_url)right.push(h("button",{class:"btn sm gh fwu-notes-btn",onclick:()=>notesModal(self.latest.notes_url,tg?tg.version:self.latest.version,"self")},"Release notes"));
    if(self.available&&tg&&self.installable)right.push(h("button",{class:"btn pri sm fwu-update",disabled:!!(job&&job.active)||mobile(),
      title:mobile()?"Downloads need a WiFi connection":"",onclick:()=>startInstall(tg)},ic("download"),"Update"));
    else if(self.available&&tg&&(tg.verdict==="serial"||tg.verdict==="unknown"))right.push(h("a",{class:"btn sm fwu-usb",href:DOCS_URL,target:"_blank"},"Needs a USB flash"));
    const installed=h("span",{class:"mono",title:"build "+(self.build||"")},self.installed||"-",self.dev_build?[" ",chip("Development build","warn")]:null,
      self.installed_yanked?[" ",chip("withdrawn","warn")]:null);
    return h("div",{class:"fwu-row",style:"display:grid;grid-template-columns:minmax(0,1fr) auto;gap:4px 12px;align-items:center;padding:10px 4px;border-top:1px solid var(--border)"},
      h("div",{},h("div",{},h("b",{style:"font-size:13.5px"},"WiCAN PRO")," ",installed),second,
        self.installed_yanked?h("div",{class:"help",style:"color:var(--warning)"},"This release was withdrawn: "+(self.yank_reason||"")):null,
        tg&&tg.verdict==="wipe"?h("div",{class:"help",style:"color:var(--warning)"},"The new partition layout erases "+(tg.wipes||[]).join(", ")+" on the way: a backup first."):null,
        tg&&tg.verdict==="lossless"?h("div",{class:"help"},"The first start after the update restarts once more to update the partition layout."):null),
      h("div",{class:"rowflex",style:"gap:8px;justify-content:flex-end"},...right));
  }

  function dongleRow(){
    const d=st.targets.dongle||{},job=st.job;
    const title=h("b",{style:"font-size:13.5px"+(d.verdict==="not_built"||d.verdict==="not_present"?";color:var(--text-3)":"")},"ESPNetLink");
    if(d.verdict==="not_built")return h("div",{class:"fwu-row fwu-dongle",style:"padding:10px 4px;border-top:1px solid var(--border)"},
      h("div",{},title," ",h("span",{class:"help"},"updating the dongle from here comes in a later firmware; use its own web page for now")));
    if(!d.present)return h("div",{class:"fwu-row fwu-dongle",style:"padding:10px 4px;border-top:1px solid var(--border)"},
      h("div",{},title," ",h("span",{class:"help fwu-dongle-reason"},DONGLE_VERDICT_TEXT.not_present)));
    const inst=d.installed||{};
    const installed=h("span",{class:"mono",title:"device "+(inst.device_id||"")+", api "+(inst.api_level||"?")},inst.fw_version||"not identified yet",
      inst.dev_build?[" ",chip("Development build","warn")]:null,
      d.reachable?[" ",chip(d.transport==="usb"?"USB":"WiFi","ok")]:[" ",chip("not reachable","warn")]);
    const second=d.available&&d.target_version
      ?h("div",{class:"help fwu-dongle-avail"},d.target_version+" available"+(d.latest&&d.latest.date?", "+d.latest.date:"")+(d.latest&&d.latest.summary?", "+d.latest.summary:""))
      :h("div",{class:"help fwu-dongle-reason"},DONGLE_VERDICT_TEXT[d.verdict]||REASON_TEXT[d.reason]||d.verdict||"Up to date");
    const right=[];
    if(d.latest&&d.latest.notes_url)right.push(h("button",{class:"btn sm gh fwu-dongle-notes",onclick:()=>notesModal(d.latest.notes_url,d.target_version||d.latest.version,"dongle")},"Release notes"));
    if(d.verdict==="update_available"&&d.installable)right.push(h("button",{class:"btn pri sm fwu-dongle-update",disabled:!!(job&&job.active)||mobile()||!d.reachable,
      title:mobile()?"Downloads need a WiFi connection":!d.reachable?"The dongle is not reachable":"",onclick:()=>startDongle(d)},ic("download"),"Update dongle"));
    else if(d.verdict==="needs_usb_flash"||d.verdict==="layout_mismatch")right.push(h("a",{class:"btn sm fwu-dongle-usb",href:DONGLE_DOCS_URL,target:"_blank"},"Needs a USB flash"));
    return h("div",{class:"fwu-row fwu-dongle",style:"display:grid;grid-template-columns:minmax(0,1fr) auto;gap:4px 12px;align-items:center;padding:10px 4px;border-top:1px solid var(--border)"},
      h("div",{},h("div",{},title," ",installed),second),
      h("div",{class:"rowflex",style:"gap:8px;justify-content:flex-end"},...right));
  }

  function render(){
    const self=st.targets.self,tg=self.target,job=st.job,last=st.last_result,d=st.targets.dongle||{};
    checked.textContent=st.last_check?(st.last_check.ok?"checked "+ago(st.last_check.age_s):"check failed "+ago(st.last_check.age_s)+" ("+(st.last_check.error||"")+")"):"not checked yet";
    checkBtn.disabled=!!(job&&job.active);
    rows.replaceChildren(wicanRow(),dongleRow());
    /* the job */
    if(job&&(job.active||["failed","aborted"].includes(job.state))){
      const dongle=job.target==="dongle",texts=dongle?DONGLE_JOB_TEXT:JOB_TEXT;
      const pct=job.total?Math.round(100*job.received/job.total):0;
      const words=job.active?(texts[job.state]||job.state)
          +(job.state==="downloading"&&job.total?" "+mb(job.received)+" of "+mb(job.total):"")
          +(job.state==="pushing"?" over "+(job.transport==="usb"?"USB":"its WiFi")+", "+pct+" %":"")
          +(job.attempt>1?" (attempt "+job.attempt+")":"")
        :(texts[job.state]||job.state)+": "+codeText(job.error)+(job.detail?" ("+job.detail+")":"");
      const tooLate=["pushing","rebooting","pending_verify"].includes(job.state);
      prog.style.display="block";
      prog.replaceChildren(
        job.active&&["downloading","pushing"].includes(job.state)?h("div",{style:"height:10px;background:var(--surface-3);border-radius:6px;overflow:hidden"},h("i",{style:"display:block;height:100%;width:"+pct+"%;background:var(--primary);border-radius:6px;transition:width .25s"})):null,
        h("div",{class:"rowflex",style:"justify-content:space-between;margin-top:7px;gap:10px"},
          h("span",{class:"help fwu-job",style:job.active?"":"color:var(--danger)"},(job.active?"":"Last attempt: ")+words+(job.active&&job.state==="downloading"?" "+pct+" %":"")),
          job.active&&!tooLate&&job.state!=="rebooting"?h("button",{class:"btn sm fwu-cancel",onclick:async()=>{try{await post("/api/fw_update/abort");}catch(e){toast(e.message,"err");}}},"Cancel"):null));
      if(job.state==="rebooting"&&!dongle){expectReboot("Installing firmware");}
    }else{prog.style.display="none";prog.replaceChildren();}
    /* the last recorded job of this device */
    result.replaceChildren();
    if(last&&last.state==="pending_verify")result.append(banner("info","info",h("b",{},"Updated to "+last.to+", verifying.")," The new firmware is confirmed after ten minutes without a crash, or at the next planned restart; three quick crashes roll it back to "+last.from+"."));
    else if(last&&last.state==="rolled_back")result.append(banner("crit","alert",h("b",{},"Rolled back to "+last.from+(last.by==="bootloader"?" by the bootloader":" after repeated crashes")+".")," Version "+last.to+" did not run well on this device.",
      self.available&&tg?[" ",h("button",{class:"btn sm fwu-retry",onclick:()=>startInstall(tg)},"Try again")]:null));
    else if(last&&last.state==="boot_failed")result.append(banner("crit","alert",h("b",{},"Version "+last.to+" did not start.")," The device kept "+last.from+"."));
    else if(last&&last.state==="verified"&&last.to===self.installed)result.append(h("div",{class:"help fwu-verified",style:"margin-top:8px"},"Updated to "+last.to+" from "+last.from+", verified."));
    /* the last dongle job (this boot) */
    const dl=d.last_result;
    if(dl&&!(job&&job.active&&job.target==="dongle")){
      if(dl.state==="verified")result.append(h("div",{class:"help fwu-dongle-verified",style:"margin-top:8px"},"Dongle updated to "+dl.to+(dl.from?" from "+dl.from:"")+(dl.push_s?" (sent in "+dl.push_s+" s)":"")+"."));
      else if(dl.state==="failed")result.append(banner("crit","alert",h("b",{},"The dongle update to "+dl.to+" failed: ")," "+codeText(dl.error)+(dl.detail?" ("+dl.detail+")":"")+".",
        d.verdict==="update_available"&&d.installable&&!mobile()?[" ",h("button",{class:"btn sm fwu-dongle-retry",onclick:()=>startDongle(d)},"Try again")]:null));
    }
  }

  async function refresh(){try{st=await api("/api/fw_update");render();}catch(e){/* a restart in flight: keep the last view */}}
  function pollUntilIdle(){
    if(timer)clearInterval(timer);
    timer=setInterval(async()=>{
      await refresh();
      if(!document.body.contains(rows)){clearInterval(timer);timer=null;return;}
      if(!(st.job&&st.job.active)){clearInterval(timer);timer=null;}
    },2000);
  }

  render();
  if(st.job&&st.job.active)pollUntilIdle();
  const el=cardTools("Updates","download",[checked,checkBtn],
    h("div",{class:"help",style:"margin-bottom:6px"},"Releases come from the MeatPi release list for this device and its dongle: channel ",h("b",{},st.channel),
      ". Downloads go over WiFi only, never over the ESPNetLink's mobile connection. ",
      h("a",{href:DOCS_URL,target:"_blank"},"How updates work")),
    mobile()?banner("warn","alert",h("b",{},"Online through the mobile connection.")," Updates are listed, but downloads wait until the device is on a WiFi network."):null,
    rows,prog,result,
    h("div",{style:"margin-top:14px;padding-top:10px;border-top:1px solid var(--border)"},
      h("div",{class:"help",style:"margin-bottom:6px"},"Update settings (restart to apply)"),settingsRows));
  el.id="fwu-card";
  return el;
}
PAGES.__fwupdate={card,mdLite};
})();
