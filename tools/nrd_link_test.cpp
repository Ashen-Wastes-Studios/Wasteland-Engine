// Link-level proof that NRD.lib resolves and carries the shader blobs the
// engine will need. Deliberately minimal: no engine headers, so it compiles
// independently of the engine's WIP.
//
// Build:
//   cl /EHsc /std:c++17 /I Wasteland/vendor/NRD/Include nrd_link_test.cpp
//      /link /LIBPATH:Wasteland/vendor/NRD/_Bin/Release NRD.lib
#include <NRD.h>

#include <cstdio>
#include <cstdlib>

int main()
{
    nrd::AllocationCallbacks cb{};
        cb.Allocate = [](void *, size_t size, size_t) -> void * { return ::operator new(size); };
        cb.Reallocate = [](void *, void *mem, size_t size, size_t) -> void *
        { return ::realloc(mem, size); };
        cb.Free = [](void *, void *mem) { ::free(mem); };

        nrd::DenoiserDesc denoisers[1]{};
        denoisers[0].denoiser = nrd::Denoiser::REBLUR_DIFFUSE;

    nrd::InstanceCreationDesc create{};
    create.allocationCallbacks = cb;
    create.denoisers = denoisers;
    create.denoisersNum = 1;

    nrd::Instance *inst = nullptr;
    if (nrd::CreateInstance(create, inst) != nrd::Result::SUCCESS || !inst)
    {
        printf("FAIL: CreateInstance\n");
        return 1;
    }

    const nrd::InstanceDesc &d = nrd::GetInstanceDesc(*inst);
    printf("pipelines=%u permanentPool=%u transientPool=%u samplers=%u\n",
           d.pipelinesNum, d.permanentPoolSize, d.transientPoolSize, d.samplersNum);

    // A pipeline with no bytecode would link cleanly and execute nothing,
    // which is exactly the failure a build-only check cannot see.
    if (d.pipelinesNum && d.pipelines[0].computeShaderDXIL.bytecode)
        printf("PASS: NRD linked, %u pipelines, DXIL bytecode embedded\n", d.pipelinesNum);
    else
        printf("FAIL: no embedded DXIL in pipeline 0\n");

    nrd::DestroyInstance(*inst);
    return 0;
}