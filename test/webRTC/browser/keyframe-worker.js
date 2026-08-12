"use strict";

/*
 * 接收端编码帧转换 Worker。
 * 所有帧不做修改直接送入浏览器解码器，同时提供发送关键帧请求和识别关键帧的能力。
 */
self.onrtctransform = (event) => {
    const transformer = event.transformer;
    const port = transformer.options.port;
    let pendingRequestId = 0;

    port.onmessage = async (messageEvent) => {
        const message = messageEvent.data || {};
        const requestId = Number(message.requestId) || 0;

        if (message.type !== "request-keyframe" || requestId <= 0) {
            return;
        }

        pendingRequestId = requestId;
        try {
            /*
             * 调用发生在接收端 depacketizer 上，浏览器会判断是否需要发送 RTCP FIR/PLI。
             * Promise 成功只表示请求已处理，最终是否发出报文仍由浏览器决定。
             */
            await transformer.sendKeyFrameRequest();
            port.postMessage({ type: "request-complete", requestId });
        } catch (error) {
            pendingRequestId = 0;
            port.postMessage({
                type: "request-error",
                requestId,
                error: error && error.message ? error.message : String(error),
            });
        }
    };
    port.start();

    transformer.readable
        .pipeThrough(new TransformStream({
            transform(frame, controller) {
                const metadata = typeof frame.getMetadata === "function" ? frame.getMetadata() : {};

                if (frame.type === "key") {
                    port.postMessage({
                        type: "keyframe",
                        requestId: pendingRequestId,
                        rtpTimestamp: metadata.rtpTimestamp,
                    });
                    pendingRequestId = 0;
                }
                controller.enqueue(frame);
            },
        }))
        .pipeTo(transformer.writable)
        .catch((error) => {
            port.postMessage({
                type: "transform-error",
                error: error && error.message ? error.message : String(error),
            });
        });
};
