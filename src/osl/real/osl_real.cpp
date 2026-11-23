// Nora - C ABI shim over the real OpenShadingLanguage. MIT License.
//
// Built with -DNORA_WITH_OSL=ON. With OSL found (NORA_OSL_REAL_ENABLED) the
// shim compiles shaders with OSL::OSLCompiler::compile_buffer, loads the .oso
// with ShadingSystem::LoadMemoryCompiledShader, executes them per shading
// point and walks the resulting closure tree into Nora's closure list. The
// closures registered are the ones Nora maps to its BSDFs: diffuse,
// oren_nayar, microfacet, reflection, refraction, emission, transparent.
// Without OSL the file compiles to a stub, so the flag never breaks a build.
#include "osl_real.h"
#include <cstring>
#include <cstdio>
#include <string>

#if defined(NORA_OSL_REAL_ENABLED)
#include <OSL/oslcomp.h>
#include <OSL/oslexec.h>
#include <OSL/oslclosure.h>
#include <OSL/genclosure.h>
#include <OSL/rendererservices.h>
#include <OSL/oslversion.h>
#include <mutex>

using namespace OSL;

namespace {

enum { CID_DIFFUSE = 1, CID_OREN_NAYAR, CID_MICROFACET, CID_REFLECTION, CID_REFRACTION, CID_EMISSION, CID_TRANSPARENT };

struct DiffuseParams { Vec3 N; };
struct OrenNayarParams { Vec3 N; float sigma; };
struct MicrofacetParams { ustring dist; Vec3 N, U; float xalpha, yalpha, eta; int refract; };
struct ReflectionParams { Vec3 N; float eta; };
struct RefractionParams { Vec3 N; float eta; };
struct EmptyParams { };

class CaptureErrors : public OIIO::ErrorHandler {
public:
    std::string text;
    void operator()(int errcode, const std::string &msg) override {
        if (errcode >= EH_ERROR) { if (!text.empty()) text += "\n"; text += msg; }
    }
};

struct Global {
    std::mutex mu;
    CaptureErrors errors;
    RendererServices renderer;
    ShadingSystem *ss = nullptr;
    int counter = 0;
    Global() {
        ss = new ShadingSystem(&renderer, nullptr, &errors);
        ss->attribute("lockgeom", 1);
        ClosureParam diffuse[] = { CLOSURE_VECTOR_PARAM(DiffuseParams, N), CLOSURE_FINISH_PARAM(DiffuseParams) };
        ClosureParam oren[] = { CLOSURE_VECTOR_PARAM(OrenNayarParams, N), CLOSURE_FLOAT_PARAM(OrenNayarParams, sigma),
                                CLOSURE_FINISH_PARAM(OrenNayarParams) };
        ClosureParam micro[] = { CLOSURE_STRING_PARAM(MicrofacetParams, dist), CLOSURE_VECTOR_PARAM(MicrofacetParams, N),
                                 CLOSURE_VECTOR_PARAM(MicrofacetParams, U), CLOSURE_FLOAT_PARAM(MicrofacetParams, xalpha),
                                 CLOSURE_FLOAT_PARAM(MicrofacetParams, yalpha), CLOSURE_FLOAT_PARAM(MicrofacetParams, eta),
                                 CLOSURE_INT_PARAM(MicrofacetParams, refract), CLOSURE_FINISH_PARAM(MicrofacetParams) };
        ClosureParam refl[] = { CLOSURE_VECTOR_PARAM(ReflectionParams, N), CLOSURE_FLOAT_PARAM(ReflectionParams, eta),
                                CLOSURE_FINISH_PARAM(ReflectionParams) };
        ClosureParam refr[] = { CLOSURE_VECTOR_PARAM(RefractionParams, N), CLOSURE_FLOAT_PARAM(RefractionParams, eta),
                                CLOSURE_FINISH_PARAM(RefractionParams) };
        ClosureParam empty[] = { CLOSURE_FINISH_PARAM(EmptyParams) };
        ss->register_closure("diffuse", CID_DIFFUSE, diffuse, nullptr, nullptr);
        ss->register_closure("oren_nayar", CID_OREN_NAYAR, oren, nullptr, nullptr);
        ss->register_closure("microfacet", CID_MICROFACET, micro, nullptr, nullptr);
        ss->register_closure("reflection", CID_REFLECTION, refl, nullptr, nullptr);
        ss->register_closure("refraction", CID_REFRACTION, refr, nullptr, nullptr);
        ss->register_closure("emission", CID_EMISSION, empty, nullptr, nullptr);
        ss->register_closure("transparent", CID_TRANSPARENT, empty, nullptr, nullptr);
    }
};

Global &global() { static Global g; return g; }

struct Shader {
    ShaderGroupRef group;
};

struct ThreadState {
    PerThreadInfo *pti = nullptr;
    ShadingContext *ctx = nullptr;
};
thread_local ThreadState t_state;

void set3(float *d, const Vec3 &v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; }

void flatten(const ClosureColor *c, Color3 w, osl_closure_list *out, int depth) {
    if (!c || depth > 64) return;
    switch (c->id) {
    case ClosureColor::MUL: {
        const ClosureMul *m = c->as_mul();
        flatten(m->closure, w * m->weight, out, depth + 1);
        return;
    }
    case ClosureColor::ADD: {
        const ClosureAdd *a = c->as_add();
        flatten(a->closureA, w, out, depth + 1);
        flatten(a->closureB, w, out, depth + 1);
        return;
    }
    default: break;
    }
    const ClosureComponent *comp = c->as_comp();
    Color3 cw = w * comp->w;
    if (out->n >= OSL_MAX_CLOSURES) return;
    osl_closure &o = out->c[out->n];
    std::memset(&o, 0, sizeof o);
    o.w[0] = cw.x; o.w[1] = cw.y; o.w[2] = cw.z;
    switch (comp->id) {
    case CID_DIFFUSE: o.type = OSL_CL_DIFFUSE; set3(o.N, comp->as<DiffuseParams>()->N); break;
    case CID_OREN_NAYAR: o.type = OSL_CL_DIFFUSE; set3(o.N, comp->as<OrenNayarParams>()->N); break;
    case CID_MICROFACET: {
        const MicrofacetParams *p = comp->as<MicrofacetParams>();
        o.type = OSL_CL_MICROFACET; set3(o.N, p->N);
        o.alpha = 0.5f * (p->xalpha + p->yalpha); o.eta = p->eta; o.refract = p->refract;
        break;
    }
    case CID_REFLECTION: o.type = OSL_CL_REFLECTION; set3(o.N, comp->as<ReflectionParams>()->N); o.eta = comp->as<ReflectionParams>()->eta; break;
    case CID_REFRACTION: o.type = OSL_CL_REFRACTION; set3(o.N, comp->as<RefractionParams>()->N); o.eta = comp->as<RefractionParams>()->eta; break;
    case CID_EMISSION: o.type = OSL_CL_EMISSION; break;
    case CID_TRANSPARENT: o.type = OSL_CL_TRANSPARENT; break;
    default: return;
    }
    out->n++;
}

} // namespace

extern "C" int nora_real_osl_available(void) { return 1; }
extern "C" const char *nora_real_osl_version(void) { return OSL_LIBRARY_VERSION_STRING; }

extern "C" void *nora_real_osl_compile(const char *source, char *err, size_t errcap) {
    Global &G = global();
    std::lock_guard<std::mutex> lock(G.mu);
    G.errors.text.clear();
    OSLCompiler compiler(&G.errors);
    std::string oso;
    std::vector<std::string> options;
    if (!compiler.compile_buffer(source ? source : "", oso, options, "", "nora_shader.osl")) {
        if (err && errcap) std::snprintf(err, errcap, "%s", G.errors.text.empty() ? "liboslcomp failed" : G.errors.text.c_str());
        return nullptr;
    }
    std::string name = "nora_shader_" + std::to_string(++G.counter);
    if (!G.ss->LoadMemoryCompiledShader(name, oso)) {
        if (err && errcap) std::snprintf(err, errcap, "liboslexec could not load the compiled shader: %s", G.errors.text.c_str());
        return nullptr;
    }
    Shader *s = new Shader;
    s->group = G.ss->ShaderGroupBegin(name);
    G.ss->Shader(*s->group, "surface", name, "layer1");
    G.ss->ShaderGroupEnd(*s->group);
    return s;
}

extern "C" int nora_real_osl_execute(void *shader, const osl_globals *g, osl_closure_list *out) {
    Global &G = global();
    Shader *s = static_cast<Shader *>(shader);
    if (!s || !g || !out) return -1;
    out->n = 0;
    if (!t_state.ctx) {
        t_state.pti = G.ss->create_thread_info();
        t_state.ctx = G.ss->get_context(t_state.pti);
    }
    ShaderGlobals sg;
    std::memset((void *)&sg, 0, sizeof sg);
    sg.P = Vec3(g->P[0], g->P[1], g->P[2]);
    sg.N = Vec3(g->N[0], g->N[1], g->N[2]);
    sg.Ng = Vec3(g->Ng[0], g->Ng[1], g->Ng[2]);
    sg.I = Vec3(g->I[0], g->I[1], g->I[2]);
    sg.dPdu = Vec3(g->dPdu[0], g->dPdu[1], g->dPdu[2]);
    sg.dPdv = Vec3(g->dPdv[0], g->dPdv[1], g->dPdv[2]);
    sg.u = g->u;
    sg.v = g->v;
    sg.time = g->time;
    sg.backfacing = g->backfacing;
    sg.surfacearea = 1.0f;
    sg.raytype = 1;   // camera
    if (!G.ss->execute(*t_state.ctx, *s->group, sg)) return -1;
    flatten(sg.Ci, Color3(1.0f), out, 0);
    return 0;
}

extern "C" void nora_real_osl_free(void *shader) { delete static_cast<Shader *>(shader); }

#else  // ------------------------------------------------------------- stub

extern "C" int nora_real_osl_available(void) { return 0; }
extern "C" const char *nora_real_osl_version(void) { return "not available (built without OpenShadingLanguage)"; }
extern "C" void *nora_real_osl_compile(const char *source, char *err, size_t errcap) {
    (void)source;
    if (err && errcap) std::snprintf(err, errcap, "Nora was built with NORA_WITH_OSL=ON but OpenShadingLanguage was not found; using the built-in compiler");
    return nullptr;
}
extern "C" int nora_real_osl_execute(void *shader, const osl_globals *g, osl_closure_list *out) {
    (void)shader; (void)g;
    if (out) out->n = 0;
    return -1;
}
extern "C" void nora_real_osl_free(void *shader) { (void)shader; }

#endif
