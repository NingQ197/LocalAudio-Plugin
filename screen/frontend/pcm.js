export class PcmPipeline extends EventTarget {
  constructor(){super();this.context=null;this.node=null;this.destination=null;this.packets=0;this.lastPacket=0;this.maxPacketBytes=19200;}
  async start(){
    this.context=new AudioContext({sampleRate:48000,latencyHint:'interactive'});
    const resumed=this.context.resume(); // invoked during the initiating click
    await this.context.audioWorklet.addModule(new URL('./pcm-worklet.js',import.meta.url));
    this.node=new AudioWorkletNode(this.context,'native-pcm',{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[1]});
    this.destination=this.context.createMediaStreamDestination();this.node.connect(this.destination);
    this.node.port.onmessage=event=>this.dispatchEvent(new CustomEvent('metrics',{detail:event.data}));
    await resumed;return this.destination.stream.getAudioTracks()[0];
  }
  push(packet){
    if(!this.node||packet.rate!==48000||packet.channels!==1||typeof packet.base64!=='string'||packet.base64.length>this.maxPacketBytes*1.4)return false;
    try{const decoded=atob(packet.base64);if(decoded.length%2||decoded.length>this.maxPacketBytes)return false;
      const bytes=new Uint8Array(decoded.length);for(let i=0;i<decoded.length;i++)bytes[i]=decoded.charCodeAt(i);
      this.node.port.postMessage({type:'pcm',buffer:bytes.buffer,rate:48000,channels:1},[bytes.buffer]);this.packets++;this.lastPacket=performance.now();return true;
    }catch{return false;}
  }
  async stop(){const context=this.context;this.context=null;if(this.node){this.node.port.postMessage({type:'flush'});this.node.disconnect();this.node=null;}
    for(const t of this.destination?.stream.getTracks()||[])t.stop();this.destination=null;if(context&&context.state!=='closed')await context.close();}
}
