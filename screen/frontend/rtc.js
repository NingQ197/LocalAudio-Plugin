// WebRTC transport. Signalling is carried only by the authenticated TS bridge.
export const DEFAULT_NETWORK = Object.freeze({stunUrls:[],turnUrls:[],turnUsername:'',turnCredential:'',iceTransportPolicy:'all',maxViewers:4});
const KINDS = new Set(['discover','hostannounce','stop','viewrequest','approve','approveack','deny','offer','answer','ice','leave','restart','peer-leave']);
const uuid=()=>crypto.randomUUID();
const keyOf=value=>String(value??'');
const validID=value=>typeof value==='string'&&/^[a-zA-Z0-9_-]{8,80}$/.test(value);
const urls=value=>Array.isArray(value)?value.map(String).filter(Boolean):String(value||'').split(/[\s,]+/).filter(Boolean);
export function normalizeNetwork(raw={}) {
  const out={...DEFAULT_NETWORK,...raw};
  out.stunUrls=urls(raw.stunUrls??DEFAULT_NETWORK.stunUrls).filter(x=>/^stuns?:[^\s]+$/i.test(x));
  out.turnUrls=urls(raw.turnUrls).filter(x=>/^turns?:[^\s]+$/i.test(x));
  out.iceTransportPolicy=raw.iceTransportPolicy==='relay'?'relay':'all';
  out.turnUsername=String(raw.turnUsername||'');out.turnCredential=String(raw.turnCredential||'');
  out.maxViewers=Math.min(6,Math.max(1,Number(raw.maxViewers)||4));return out;
}
export function turnCredentialStatus(raw={},now=Math.floor(Date.now()/1000)) {
  const n=normalizeNetwork(raw);
  if(!n.turnUsername.trim()||!n.turnCredential.trim())return 'missing';
  const expiry=/^(\d+):/.exec(n.turnUsername.trim());
  if(expiry&&Number(expiry[1])<=now)return 'expired';
  return 'valid';
}
export function peerConfiguration(raw) {
  const n=normalizeNetwork(raw),iceServers=[];
  if(n.stunUrls.length)iceServers.push({urls:n.stunUrls});
  const auth=turnCredentialStatus(n);
  if(n.turnUrls.length&&auth==='valid')iceServers.push({urls:n.turnUrls,username:n.turnUsername,credential:n.turnCredential});
  if(n.iceTransportPolicy==='relay'){
    if(!n.turnUrls.length)throw Error('强制中继需要先配置 TURN 服务器。');
    if(auth==='missing')throw Error('仅使用 TURN 中继需要填写服务器生成的用户名和密码。');
    if(auth==='expired')throw Error('TURN 临时凭据已过期，请重新生成后再连接。');
  }
  return {iceServers,iceTransportPolicy:n.iceTransportPolicy,bundlePolicy:'max-bundle'};
}
export class ShareSession extends EventTarget {
  constructor({send,broadcast,configuration=()=>DEFAULT_NETWORK,peerFactory=c=>new RTCPeerConnection(c),now=()=>Date.now()}={}) {
    super();this.sendNative=send;this.broadcastNative=broadcast;this.configuration=configuration;this.peerFactory=peerFactory;this.now=now;
    this.context={selfID:0,handler:0,channelID:0,peers:[]};this.host=null;this.view=null;this.hosts=new Map();this.requests=new Map();this.connections=new Map();this.disposed=false;this.lastDiscover=0;
    this.heartbeat=setInterval(()=>this.maintain(),5000);this.statsTimer=setInterval(()=>this.collectStats(),2000);
  }
  emit(type,detail={}) {this.dispatchEvent(new CustomEvent(type,{detail}));}
  known(id) {return this.context.peers.some(p=>keyOf(p.id)===keyOf(id))&&keyOf(id)!==keyOf(this.context.selfID);}
  connected() {return Boolean(this.context.handler&&this.context.channelID&&this.context.selfID);}
  envelope(kind,sessionID,extra={}) {return {v:1,kind,sessionID,channelID:this.context.channelID,...extra};}
  send(to,kind,sessionID,extra={}) {if(this.known(to))this.sendNative?.(to,this.envelope(kind,sessionID,extra));}
  broadcast(kind,sessionID,extra={}) {if(this.connected())this.broadcastNative?.(this.envelope(kind,sessionID,extra));}
  updateContext(next) {
    const changed=this.context.handler&&(keyOf(next.handler)!==keyOf(this.context.handler)||keyOf(next.channelID)!==keyOf(this.context.channelID)||keyOf(next.selfID)!==keyOf(this.context.selfID));
    if(changed){this.stopHost('频道已切换');this.stopViewing('频道已切换');this.hosts.clear();}
    this.context={...this.context,...next,peers:Array.isArray(next.peers)?next.peers:[]};
    for(const [id] of this.connections)if(!this.known(id))this.removeConnection(id,'成员已离开频道');
    for(const [id] of this.requests)if(!this.known(id))this.requests.delete(id);
    for(const [id] of this.hosts)if(!this.known(id))this.hosts.delete(id);
    if(this.view&&!this.known(this.view.hostID))this.stopViewing('分享者已离开频道');
    this.emit('hosts',{hosts:this.availableHosts()});this.emitRequests();
    if(this.connected()&&this.now()-this.lastDiscover>5000){this.lastDiscover=this.now();this.broadcast('discover',uuid());}
  }
  availableHosts(){return [...this.hosts.values()].sort((a,b)=>a.name.localeCompare(b.name));}
  announcement(){if(!this.host)return {};const s=this.host.settings;return {title:'屏幕共享',settings:{width:s.width||0,height:s.height||0,frameRate:s.frameRate||0},audio:this.host.audio,startedAt:this.host.startedAt};}
  startHost(stream,{title='屏幕共享',settings={},audio=false,quality=null}={}) {
    if(!this.connected())throw Error('先连接 TeamSpeak 服务器并加入频道。');
    if(this.host||this.view)throw Error('请先结束当前共享或观看。');
    if(!stream?.getVideoTracks().some(t=>t.readyState==='live'))throw Error('没有可用的屏幕画面。');
    peerConfiguration(this.configuration());
    this.host={sessionID:uuid(),stream,title:String(title).slice(0,100),settings,audio,quality:quality||{height:settings.height||0,frameRate:settings.frameRate||30},startedAt:this.now()};
    const sid=this.host.sessionID;
    for(const track of stream.getVideoTracks())track.addEventListener('ended',()=>{if(this.host?.sessionID===sid)this.stopHost('屏幕捕获已结束');},{once:true});
    this.broadcast('hostannounce',sid,this.announcement());this.emit('host',{host:this.host});return sid;
  }
  stopHost(reason='已停止共享') {
    if(!this.host)return;const old=this.host;this.host=null;
    this.broadcast('stop',old.sessionID,{reason});
    for(const [id,r]of [...this.connections])if(r.role==='host')this.removeConnection(id,reason);
    this.requests.clear();for(const track of old.stream.getTracks())track.stop();
    this.emitRequests();this.emit('hoststop',{reason});
  }
  requestView(hostID,sessionID) {
    if(!this.connected())throw Error('先连接 TeamSpeak 频道。');
    if(this.host)throw Error('请先停止自己的屏幕共享。');
    const host=this.hosts.get(keyOf(hostID));if(!host||host.sessionID!==sessionID||!this.known(hostID))throw Error('这次分享已结束，请刷新列表。');
    peerConfiguration(this.configuration());this.stopViewing();
    this.view={hostID:keyOf(hostID),sessionID,requestID:uuid(),token:null,requestedAt:this.now(),approved:false};
    this.send(hostID,'viewrequest',sessionID,{requestID:this.view.requestID});this.emit('viewstate',{state:'requested',host});
  }
  approve(id) {
    id=keyOf(id);const request=this.requests.get(id);if(!this.host||!request||!this.known(id))return false;
    const count=[...this.connections.values()].filter(r=>r.role==='host'&&r.id!==id).length;
    if(count>=normalizeNetwork(this.configuration()).maxViewers)throw Error('观看人数已达上限，请先断开一位观看者。');
    this.requests.delete(id);this.removeConnection(id,'重新授权');
    const r=this.makeRecord(id,'host',this.host.sessionID,request.requestID,uuid());
    this.send(id,'approve',r.sessionID,{requestID:r.requestID,token:r.token});this.emitRequests();return true;
  }
  deny(id,reason='暂时无法加入观看') {
    id=keyOf(id);const request=this.requests.get(id);if(!request||!this.host)return;
    this.send(id,'deny',this.host.sessionID,{requestID:request.requestID,reason});this.requests.delete(id);this.emitRequests();
  }
  emitRequests(){this.emit('requests',{requests:[...this.requests.values()],viewers:[...this.connections.values()].filter(r=>r.role==='host').map(r=>({id:r.id,name:r.name,state:r.pc?.connectionState||'正在连接'}))});}
  makeRecord(id,role,sessionID,requestID,token) {
    const r={id:keyOf(id),name:this.context.peers.find(p=>keyOf(p.id)===keyOf(id))?.name||`成员 ${id}`,role,sessionID,requestID,token,pc:null,queue:Promise.resolve(),ice:new Map(),negotiationID:null,createdAt:this.now(),restartAttempts:0,remoteStream:null,lastStats:null,disconnectedAt:0};
    this.connections.set(r.id,r);return r;
  }
  active(r){return !this.disposed&&this.connections.get(r.id)===r&&((r.role==='host'&&this.host?.sessionID===r.sessionID)||(r.role==='viewer'&&this.view?.sessionID===r.sessionID&&this.view?.token===r.token));}
  authorised(r,p){return r&&p.sessionID===r.sessionID&&p.requestID===r.requestID&&p.token===r.token;}
  serialize(r,job){r.queue=r.queue.then(async()=>{if(this.active(r))await job();}).catch(e=>{if(this.active(r))this.emit('error',{message:`与 ${r.name} 协商失败：${e.message}`});});return r.queue;}
  createPeer(r) {
    if(r.pc)return r.pc;
    const pc=this.peerFactory(peerConfiguration(this.configuration()));r.pc=pc;
    pc.onicecandidate=event=>{if(this.active(r)&&r.negotiationID)this.send(r.id,'ice',r.sessionID,{requestID:r.requestID,token:r.token,negotiationID:r.negotiationID,candidate:event.candidate?.toJSON?.()??event.candidate??null});};
    pc.onconnectionstatechange=()=>{
      if(!this.active(r))return;const state=pc.connectionState;
      if(state==='connected'){r.disconnectedAt=0;r.restartAttempts=0;}
      if(state==='disconnected')r.disconnectedAt=this.now();
      if(state==='failed')this.restart(r.id);
      this.emit('connection',{id:r.id,name:r.name,role:r.role,state});this.emitRequests();
    };
    pc.ontrack=event=>{
      if(!this.active(r)||r.role!=='viewer')return;
      if(!r.remoteStream)r.remoteStream=new MediaStream();
      if(!r.remoteStream.getTracks().some(t=>t.id===event.track.id))r.remoteStream.addTrack(event.track);
      this.emit('remote',{stream:r.remoteStream,hostID:r.id,name:r.name});
    };
    if(r.role==='host')for(const track of this.host.stream.getTracks())pc.addTrack(track,this.host.stream);
    return pc;
  }
  sendWith(r,kind,extra={}){this.send(r.id,kind,r.sessionID,{requestID:r.requestID,token:r.token,...extra});}
  async offer(r,restart=false) {
    if(!this.active(r))return;const pc=this.createPeer(r);
    if(pc.signalingState!=='stable')return;
    r.negotiationID=uuid();
    const offer=await pc.createOffer(restart?{iceRestart:true}:{});if(!this.active(r))return;
    await pc.setLocalDescription(offer);if(!this.active(r))return;
    await this.applyVideoLimits(r);if(!this.active(r))return;
    this.sendWith(r,'offer',{negotiationID:r.negotiationID,description:pc.localDescription.toJSON?.()??{type:pc.localDescription.type,sdp:pc.localDescription.sdp}});
  }
  async applyVideoLimits(r){
    if(!this.active(r)||r.role!=='host'||!r.pc?.getSenders||!this.host)return;
    const quality=this.host.quality||{};
    for(const sender of r.pc.getSenders()){
      if(sender.track?.kind!=='video')continue;
      const settings=sender.track.getSettings(),parameters=sender.getParameters();if(!parameters.encodings?.length)continue;
      const scale=quality.height?Math.max(1,(settings.height||quality.height)/quality.height):1;
      const fps=Math.min(60,Math.max(5,Number(quality.frameRate)||30));
      const pixels=(settings.width||1920)*(settings.height||1080)/(scale*scale);
      for(const encoding of parameters.encodings){encoding.maxFramerate=fps;encoding.scaleResolutionDownBy=scale;encoding.maxBitrate=Math.min(16000000,Math.max(800000,Math.round(pixels*fps*.12)));}
      parameters.degradationPreference='maintain-resolution';await sender.setParameters(parameters);
    }
  }
  async setVideoQuality(quality){
    if(!this.host)return;this.host.quality={height:Math.max(0,Number(quality.height)||0),frameRate:Math.min(60,Math.max(5,Number(quality.frameRate)||30))};
    await Promise.all([...this.connections.values()].filter(r=>r.role==='host').map(r=>this.serialize(r,()=>this.applyVideoLimits(r))));
  }
  queueIce(r,p) {
    if(!validID(p.negotiationID))return;
    if(p.candidate!==null&&(typeof p.candidate!=='object'||typeof p.candidate.candidate!=='string'||p.candidate.candidate.length>4096))return;
    if(!r.ice.has(p.negotiationID)){if(r.ice.size>=3)r.ice.delete(r.ice.keys().next().value);r.ice.set(p.negotiationID,[]);}
    const queued=r.ice.get(p.negotiationID);if(queued.length<128)queued.push(p.candidate);
  }
  async flushIce(r) {
    if(!r.pc?.remoteDescription||!r.negotiationID)return;
    const list=r.ice.get(r.negotiationID)||[];r.ice.delete(r.negotiationID);
    for(const candidate of list){if(!this.active(r))return;try{await r.pc.addIceCandidate(candidate);}catch(e){this.emit('notice',{message:`部分网络候选已过期：${e.message}`});}}
  }
  async handleSignal({from,name,payload:p}) {
    from=keyOf(from);
    if(p?.kind==='peer-leave'&&this.known(from)&&keyOf(p.channelID)===keyOf(this.context.channelID)){this.hosts.delete(from);this.requests.delete(from);this.removeConnection(from,'对方已关闭共享窗口');if(this.view?.hostID===from)this.stopViewing('分享者已关闭窗口',false);this.emit('hosts',{hosts:this.availableHosts()});return true;}
    if(this.disposed||!this.connected()||!this.known(from)||!p||typeof p!=='object'||p.v!==1||!KINDS.has(p.kind)||!validID(p.sessionID)||keyOf(p.channelID)!==keyOf(this.context.channelID))return false;
    if(p.kind==='discover'){if(this.host)this.send(from,'hostannounce',this.host.sessionID,this.announcement());return true;}
    if(p.kind==='hostannounce'){
      if(this.host?.sessionID===p.sessionID)return false;
      this.hosts.set(from,{id:from,name:String(name||this.context.peers.find(x=>keyOf(x.id)===from)?.name||`成员 ${from}`),sessionID:p.sessionID,title:String(p.title||'屏幕共享').slice(0,100),settings:p.settings&&typeof p.settings==='object'?p.settings:{},audio:Boolean(p.audio),seenAt:this.now()});
      this.emit('hosts',{hosts:this.availableHosts()});return true;
    }
    if(p.kind==='stop'){
      if(this.hosts.get(from)?.sessionID===p.sessionID)this.hosts.delete(from);
      if(this.view?.hostID===from&&this.view.sessionID===p.sessionID)this.stopViewing('分享者已停止共享',false);
      this.emit('hosts',{hosts:this.availableHosts()});return true;
    }
    if(p.kind==='viewrequest') {
      if(!this.host||p.sessionID!==this.host.sessionID||!validID(p.requestID))return false;
      const current=this.connections.get(from);if(current&&current.requestID===p.requestID){this.sendWith(current,'approve');return true;}
      // Joining is automatic for authenticated members of this TS channel.
      // Keep the token/ack handshake so media still belongs to this exact join.
      this.requests.set(from,{id:from,name:String(name||`成员 ${from}`),requestID:p.requestID,receivedAt:this.now()});
      try{this.approve(from);}catch(error){this.deny(from,error.message);}
      return true;
    }
    if(p.kind==='deny'){
      if(this.view?.hostID===from&&this.view.sessionID===p.sessionID&&this.view.requestID===p.requestID){this.stopViewing(String(p.reason||'暂时无法加入观看'),false);return true;}return false;
    }
    if(p.kind==='approve') {
      const v=this.view;if(!v||v.hostID!==from||v.sessionID!==p.sessionID||v.requestID!==p.requestID||!validID(p.token))return false;
      if(v.approved&&v.token!==p.token)return false;
      v.approved=true;v.token=p.token;
      let r=this.connections.get(from);if(!r)r=this.makeRecord(from,'viewer',v.sessionID,v.requestID,v.token);
      this.sendWith(r,'approveack');this.emit('viewstate',{state:'approved',host:this.hosts.get(from)});return true;
    }
    const r=this.connections.get(from);if(!this.authorised(r,p)||!this.active(r)){
      // A pending request can be cancelled before approval; no token is involved.
      if(p.kind==='leave'&&this.host?.sessionID===p.sessionID&&this.requests.get(from)?.requestID===p.requestID){this.requests.delete(from);this.emitRequests();return true;}return false;
    }
    if(p.kind==='leave'){this.removeConnection(from,'对方已断开');return true;}
    if(p.kind==='approveack'&&r.role==='host') {if(!r.pc)await this.serialize(r,()=>this.offer(r));return true;}
    if(p.kind==='restart'){if(r.role==='host')this.restart(from);return true;}
    if(p.kind==='ice') {
      this.queueIce(r,p);await this.serialize(r,()=>this.flushIce(r));return true;
    }
    if(p.kind==='offer'&&r.role==='viewer') {
      if(!validID(p.negotiationID)||p.description?.type!=='offer'||typeof p.description.sdp!=='string'||p.description.sdp.length>262144)return false;
      await this.serialize(r,async()=>{
        const pc=this.createPeer(r);r.negotiationID=p.negotiationID;
        await pc.setRemoteDescription(p.description);if(!this.active(r))return;
        await this.flushIce(r);const answer=await pc.createAnswer();if(!this.active(r))return;
        await pc.setLocalDescription(answer);if(!this.active(r))return;
        this.sendWith(r,'answer',{negotiationID:r.negotiationID,description:pc.localDescription.toJSON?.()??{type:pc.localDescription.type,sdp:pc.localDescription.sdp}});
      });return true;
    }
    if(p.kind==='answer'&&r.role==='host') {
      if(p.negotiationID!==r.negotiationID||p.description?.type!=='answer'||typeof p.description.sdp!=='string'||p.description.sdp.length>262144)return false;
      await this.serialize(r,async()=>{if(r.pc?.signalingState!=='have-local-offer')return;await r.pc.setRemoteDescription(p.description);await this.flushIce(r);});return true;
    }
    return false;
  }
  restart(id) {
    const r=this.connections.get(keyOf(id));if(!r||!this.active(r))return;
    if(r.role==='viewer'){this.sendWith(r,'restart');this.emit('viewstate',{state:'restarting'});return;}
    if(r.restartAttempts>=2){this.sendWith(r,'leave');this.removeConnection(r.id,'连接中断，请重新点击观看');return;}
    r.restartAttempts++;this.serialize(r,()=>this.offer(r,true));this.emit('notice',{message:`正在重新连接 ${r.name}…`});
  }
  removeConnection(id,reason='已断开') {
    id=keyOf(id);const r=this.connections.get(id);if(!r)return;this.connections.delete(id);
    if(r.pc){r.pc.onicecandidate=null;r.pc.ontrack=null;r.pc.onconnectionstatechange=null;r.pc.close();}
    for(const t of r.remoteStream?.getTracks()||[])t.stop();r.ice.clear();
    if(r.role==='viewer'&&this.view?.hostID===id){this.view=null;this.emit('viewstop',{reason});}
    this.emitRequests();this.emit('connection',{id,role:r.role,state:'closed',reason});
  }
  disconnectViewer(id){const r=this.connections.get(keyOf(id));if(r?.role==='host'){this.sendWith(r,'leave');this.removeConnection(id,'已移除观看者');}}
  stopViewing(reason='已停止观看',notify=true) {
    const v=this.view;if(!v)return;this.view=null;
    if(notify)this.send(v.hostID,'leave',v.sessionID,{requestID:v.requestID,token:v.token});
    this.removeConnection(v.hostID,reason);this.emit('viewstop',{reason});
  }
  maintain() {
    if(this.disposed)return;const now=this.now();
    if(this.host)this.broadcast('hostannounce',this.host.sessionID,this.announcement());
    for(const[id,h]of this.hosts)if(now-h.seenAt>17000)this.hosts.delete(id);
    for(const[id,r]of this.requests)if(now-r.receivedAt>60000)this.requests.delete(id);
    for(const[id,r]of [...this.connections]){
      if(!r.pc&&now-r.createdAt>30000){this.sendWith(r,'leave');this.removeConnection(id,'连接确认超时，请重新点击观看');}
      else if(r.disconnectedAt&&now-r.disconnectedAt>10000){r.disconnectedAt=now;this.restart(id);}
    }
    if(this.view&&!this.view.approved&&now-this.view.requestedAt>60000)this.stopViewing('连接超时，请重新点击观看');
    this.emit('hosts',{hosts:this.availableHosts()});this.emitRequests();
  }
  async collectStats() {
    const result=[];
    for(const r of this.connections.values()){
      if(!r.pc||!this.active(r))continue;
      try{
        const all=await r.pc.getStats();if(!this.active(r))continue;
        let pair;for(const s of all.values())if(s.type==='transport'&&s.selectedCandidatePairId)pair=all.get(s.selectedCandidatePairId);
        if(!pair)for(const s of all.values())if(s.type==='candidate-pair'&&s.state==='succeeded'&&s.nominated)pair=s;
        const local=pair&&all.get(pair.localCandidateId),remote=pair&&all.get(pair.remoteCandidateId);
        const types=[local?.candidateType,remote?.candidateType];const route=types.includes('relay')?'relay':types.some(x=>x==='srflx'||x==='prflx')?'srflx':types.some(Boolean)?'host':'unknown';
        let video,audio;for(const s of all.values()){
          if(s.type===(r.role==='host'?'outbound-rtp':'inbound-rtp')&&!s.isRemote){if((s.kind||s.mediaType)==='video')video=s;else if((s.kind||s.mediaType)==='audio')audio=s;}
        }
        const bytes=video?.bytesSent??video?.bytesReceived??0,stamp=video?.timestamp??performance.now();
        const bitrate=r.lastStats&&stamp>r.lastStats.stamp?(bytes-r.lastStats.bytes)*8/(stamp-r.lastStats.stamp):0;r.lastStats={bytes,stamp};
        result.push({id:r.id,name:r.name,role:r.role,state:r.pc.connectionState,route,localCandidate:local?.candidateType||'—',remoteCandidate:remote?.candidateType||'—',protocol:local?.protocol||'—',width:video?.frameWidth||0,height:video?.frameHeight||0,fps:video?.framesPerSecond||0,bitrateKbps:Math.max(0,bitrate),frames:video?.framesDecoded??video?.framesEncoded??0,audioPackets:audio?.packetsReceived??audio?.packetsSent??0,audioEnergy:audio?.totalAudioEnergy??0,roundTripTimeMs:(pair?.currentRoundTripTime||0)*1000});
      }catch(e){if(this.active(r))this.emit('notice',{message:`暂时无法读取连接状态：${e.message}`});}
    }
    this.latestStats=result;this.emit('stats',{connections:result});return result;
  }
  dispose(){this.stopHost();this.stopViewing();this.disposed=true;clearInterval(this.heartbeat);clearInterval(this.statsTimer);this.hosts.clear();}
}
