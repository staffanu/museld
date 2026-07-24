repro: repro.cpp repro.spv
	g++ -O1 -o repro repro.cpp -lvulkan

repro.spv: repro.comp
	glslc -fshader-stage=compute repro.comp -o repro.spv

run: repro
	./repro repro.spv

clean:
	rm -f repro repro.spv

.PHONY: run clean
