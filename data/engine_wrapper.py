import tensorrt as trt
import pycuda.driver as cuda
import numpy as np
import os
from typing import Dict, Any
import pycuda.autoinit

class TRTInference:
    """
    TensorRT Engine Wrapper cho DeepStream
    - Load engine từ file .engine
    - Thực hiện inference với input numpy
    """

    def __init__(self, engine_path: str):
        if not os.path.exists(engine_path):
            raise FileNotFoundError(f"Không tìm thấy engine: {engine_path}")

        self.engine_path = engine_path
        self.logger = trt.Logger(trt.Logger.ERROR)
        self.runtime = trt.Runtime(self.logger)

        with open(engine_path, "rb") as f:
            self.engine = self.runtime.deserialize_cuda_engine(f.read())

        self.context = self.engine.create_execution_context()
        self.input_names = [self.engine.get_binding_name(i) for i in range(self.engine.num_bindings) if self.engine.binding_is_input(i)]
        self.output_names = [self.engine.get_binding_name(i) for i in range(self.engine.num_bindings) if not self.engine.binding_is_input(i)]

        self.bindings = [None] * self.engine.num_bindings
        self.stream = cuda.Stream()

    def infer(self, inputs: Dict[str, np.ndarray]) -> Dict[str, np.ndarray]:
        """
        Thực hiện inference với dict input {input_name: np.ndarray}
        Trả về dict output {output_name: np.ndarray}
        """
        # Chuẩn bị input
        for i, name in enumerate(self.input_names):
            inp = inputs[name]
            inp = np.ascontiguousarray(inp)
            dtype = trt.nptype(self.engine.get_binding_dtype(name))
            inp = inp.astype(dtype)
            d_input = cuda.mem_alloc(inp.nbytes)
            cuda.memcpy_htod_async(d_input, inp, self.stream)
            self.bindings[self.engine.get_binding_index(name)] = int(d_input)

        # Chuẩn bị output
        outputs = {}
        d_outputs = {}
        for name in self.output_names:
            shape = tuple(self.context.get_binding_shape(self.engine.get_binding_index(name)))
            dtype = trt.nptype(self.engine.get_binding_dtype(name))
            size = int(np.prod(shape))
            host_mem = np.empty(shape, dtype=dtype)
            d_output = cuda.mem_alloc(host_mem.nbytes)
            self.bindings[self.engine.get_binding_index(name)] = int(d_output)
            d_outputs[name] = (d_output, host_mem)

        # Inference
        self.context.execute_async_v2(bindings=self.bindings, stream_handle=int(self.stream.handle))
        for name, (d_output, host_mem) in d_outputs.items():
            cuda.memcpy_dtoh_async(host_mem, d_output, self.stream)
        self.stream.synchronize()

        # Lấy output
        for name, (_, host_mem) in d_outputs.items():
            outputs[name] = host_mem.copy()

        return outputs

    # Thêm vào class TRTInference

    def get_engine_info(self):
        return {
            "engine_path": self.engine_path,
            "input_names": self.input_names,
            "output_names": self.output_names,
            "num_bindings": self.engine.num_bindings
        }