// Native PCM enters a bounded queue. This processor is connected ONLY to a
// MediaStreamDestination, never AudioContext.destination (no local monitoring).
class NativePcmProcessor extends AudioWorkletProcessor {
  constructor(){super();this.data=new Float32Array(32768);this.mask=32767;this.read=0;this.write=0;this.dropped=0;this.underruns=0;this.blocks=0;this.port.onmessage=event=>{
    const p=event.data;if(p.type==='flush'){this.read=this.write;return;}
    if(p.type!=='pcm'||p.rate!==48000||p.channels!==1||!(p.buffer instanceof ArrayBuffer))return;
    const input=new Int16Array(p.buffer);let first=Math.max(0,input.length-16000);
    if(this.write-this.read+input.length-first>24000){const drop=this.write-this.read;this.read=this.write;this.dropped+=Math.max(0,drop);}
    for(let i=first;i<input.length;i++)this.data[this.write++&this.mask]=input[i]/32768;
    if(this.write-this.read>9600){const next=this.write-4800;this.dropped+=Math.floor(next-this.read);this.read=next;}
  };}
  process(inputs,outputs){const out=outputs[0]?.[0];if(!out)return true;const step=48000/sampleRate;
    for(let i=0;i<out.length;i++){
      if(this.write-Math.floor(this.read)>1){const index=Math.floor(this.read),f=this.read-index;out[i]=this.data[index&this.mask]*(1-f)+this.data[(index+1)&this.mask]*f;this.read+=step;}
      else{out[i]=0;this.underruns++;}
    }
    if(++this.blocks%375===0)this.port.postMessage({type:'metrics',queuedMs:Math.max(0,this.write-this.read)/48,droppedSamples:this.dropped,underruns:this.underruns});
    return true;
  }
}
registerProcessor('native-pcm',NativePcmProcessor);
