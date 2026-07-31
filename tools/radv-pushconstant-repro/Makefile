repro: repro.cpp repro.spv
	g++ -O1 -o repro repro.cpp -lvulkan

repro.spv: repro.comp
	glslc -fshader-stage=compute repro.comp -o repro.spv

run: repro
	./repro repro.spv

# Same binary and SPIR-V on lavapipe from the same Mesa build: passes, which is
# what makes this a RADV bug rather than a bad shader or a common NIR problem.
control: repro
	VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./repro repro.spv

clean:
	rm -f repro repro.spv

.PHONY: run control clean
