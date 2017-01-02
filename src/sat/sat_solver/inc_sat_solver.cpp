/*++
Copyright (c) 2014 Microsoft Corporation

Module Name:

    inc_sat_solver.cpp

Abstract:

    incremental solver based on SAT core.

Author:

    Nikolaj Bjorner (nbjorner) 2014-7-30

Notes:

--*/

#include "solver.h"
#include "tactical.h"
#include "sat_solver.h"
#include "tactic2solver.h"
#include "aig_tactic.h"
#include "propagate_values_tactic.h"
#include "max_bv_sharing_tactic.h"
#include "card2bv_tactic.h"
#include "bit_blaster_tactic.h"
#include "simplify_tactic.h"
#include "goal2sat.h"
#include "ast_pp.h"
#include "model_smt2_pp.h"
#include "filter_model_converter.h"
#include "bit_blaster_model_converter.h"
#include "ast_translation.h"
#include "ast_util.h"
#include "propagate_values_tactic.h"

// incremental SAT solver.
class inc_sat_solver : public solver {
    ast_manager&    m;
    sat::solver     m_solver;
    goal2sat        m_goal2sat;
    params_ref      m_params;
    bool            m_optimize_model; // parameter
    expr_ref_vector m_fmls;
    expr_ref_vector m_asmsf;
    unsigned_vector m_fmls_lim;
    unsigned_vector m_asms_lim;
    unsigned_vector m_fmls_head_lim;
    unsigned            m_fmls_head;
    expr_ref_vector     m_core;
    atom2bool_var       m_map;
    model_ref           m_model;
    scoped_ptr<bit_blaster_rewriter> m_bb_rewriter;
    tactic_ref          m_preprocess;
    unsigned            m_num_scopes;
    sat::literal_vector m_asms;
    goal_ref_buffer     m_subgoals;
    proof_converter_ref m_pc;
    model_converter_ref m_mc;
    model_converter_ref m_mc0;
    expr_dependency_ref m_dep_core;
    svector<double>     m_weights;
    std::string         m_unknown;


    typedef obj_map<expr, sat::literal> dep2asm_t;
public:
    inc_sat_solver(ast_manager& m, params_ref const& p):
        m(m), m_solver(p, m.limit(), 0),
        m_params(p), m_optimize_model(false),
        m_fmls(m),
        m_asmsf(m),
        m_fmls_head(0),
        m_core(m),
        m_map(m),
        m_num_scopes(0),
        m_dep_core(m),
        m_unknown("no reason given") {
        m_params.set_bool("elim_vars", false);
        m_solver.updt_params(m_params);
        init_preprocess();
    }

    virtual ~inc_sat_solver() {}

    virtual solver* translate(ast_manager& dst_m, params_ref const& p) {
        ast_translation tr(m, dst_m);
        if (m_num_scopes > 0) {
            throw default_exception("Cannot translate sat solver at non-base level");
        }
        inc_sat_solver* result = alloc(inc_sat_solver, dst_m, p);
        expr_ref fml(dst_m);
        for (unsigned i = 0; i < m_fmls.size(); ++i) {
            fml = tr(m_fmls[i].get());
            result->m_fmls.push_back(fml);
        }
        for (unsigned i = 0; i < m_asmsf.size(); ++i) {
            fml = tr(m_asmsf[i].get());
            result->m_asmsf.push_back(fml);
        }
        return result;
    }

    virtual void set_progress_callback(progress_callback * callback) {}

    virtual lbool check_sat(unsigned num_assumptions, expr * const * assumptions) {
        return check_sat(num_assumptions, assumptions, 0, 0);
    }


    void display_weighted(std::ostream& out, unsigned sz, expr * const * assumptions, unsigned const* weights) {
        m_weights.reset();
        if (weights != 0) {
            for (unsigned i = 0; i < sz; ++i) m_weights.push_back(weights[i]);
        }
        init_preprocess();
        m_solver.pop_to_base_level();
        dep2asm_t dep2asm;
        expr_ref_vector asms(m);
        for (unsigned i = 0; i < sz; ++i) {
            expr_ref a(m.mk_fresh_const("s", m.mk_bool_sort()), m);
            expr_ref fml(m.mk_implies(a, assumptions[i]), m);
            assert_expr(fml);
            asms.push_back(a);
        }
        VERIFY(l_true == internalize_formulas());
        VERIFY(l_true == internalize_assumptions(sz, asms.c_ptr(), dep2asm));
        svector<unsigned> nweights;
        for (unsigned i = 0; i < m_asms.size(); ++i) {
            nweights.push_back((unsigned) m_weights[i]);
        }
        m_solver.display_wcnf(out, m_asms.size(), m_asms.c_ptr(), nweights.c_ptr());
    }

    lbool check_sat(unsigned sz, expr * const * assumptions, double const* weights, double max_weight) {
        m_weights.reset();
        if (weights != 0) {
            m_weights.append(sz, weights);
        }
        SASSERT(m_weights.empty() == (m_weights.c_ptr() == 0));
        m_solver.pop_to_base_level();
        dep2asm_t dep2asm;
        m_model = 0;
        lbool r = internalize_formulas();
        if (r != l_true) return r;
        r = internalize_assumptions(sz, assumptions, dep2asm);
        if (r != l_true) return r;

        r = m_solver.check(m_asms.size(), m_asms.c_ptr(), m_weights.c_ptr(), max_weight);
        switch (r) {
        case l_true:
            if (sz > 0 && !weights) {
                check_assumptions(dep2asm);
            }
            break;
        case l_false:
            // TBD: expr_dependency core is not accounted for.
            if (!m_asms.empty()) {
                extract_core(dep2asm);
            }
            break;
        default:
            break;
        }
        return r;
    }
    virtual void push() {
        internalize_formulas();
        m_solver.user_push();
        ++m_num_scopes;
        m_fmls_lim.push_back(m_fmls.size());
        m_asms_lim.push_back(m_asmsf.size());
        m_fmls_head_lim.push_back(m_fmls_head);
        if (m_bb_rewriter) m_bb_rewriter->push();
        m_map.push();
    }
    virtual void pop(unsigned n) {
        if (n > m_num_scopes) {   // allow inc_sat_solver to
            n = m_num_scopes;     // take over for another solver.
        }
        if (m_bb_rewriter) m_bb_rewriter->pop(n);
        m_map.pop(n);
        SASSERT(n <= m_num_scopes);
        m_solver.user_pop(n);
        m_num_scopes -= n;
        while (n > 0) {
            m_fmls_head = m_fmls_head_lim.back();
            m_fmls.resize(m_fmls_lim.back());
            m_fmls_lim.pop_back();
            m_fmls_head_lim.pop_back();
            m_asmsf.resize(m_asms_lim.back());
            m_asms_lim.pop_back();
            --n;
        }
    }
    virtual unsigned get_scope_level() const {
        return m_num_scopes;
    }
    virtual void assert_expr(expr * t, expr * a) {
        if (a) {
            m_asmsf.push_back(a);
            assert_expr(m.mk_implies(a, t));
        }
        else {
            assert_expr(t);
        }
    }
    virtual ast_manager& get_manager() const { return m; }
    virtual void assert_expr(expr * t) {
        TRACE("sat", tout << mk_pp(t, m) << "\n";);
        m_fmls.push_back(t);
    }
    virtual void set_produce_models(bool f) {}
    virtual void collect_param_descrs(param_descrs & r) {
        goal2sat::collect_param_descrs(r);
        sat::solver::collect_param_descrs(r);
    }
    virtual void updt_params(params_ref const & p) {
        m_params = p;
        m_params.set_bool("elim_vars", false);
        m_solver.updt_params(m_params);
        m_optimize_model = m_params.get_bool("optimize_model", false);
    }
    virtual void collect_statistics(statistics & st) const {
        if (m_preprocess) m_preprocess->collect_statistics(st);
        m_solver.collect_statistics(st);
    }
    virtual void get_unsat_core(ptr_vector<expr> & r) {
        r.reset();
        r.append(m_core.size(), m_core.c_ptr());
    }
    virtual void get_model(model_ref & mdl) {
        if (!m_model.get()) {
            extract_model();
        }
        mdl = m_model;
    }
    virtual proof * get_proof() {
        UNREACHABLE();
        return 0;
    }

    virtual lbool get_consequences_core(expr_ref_vector const& assumptions, expr_ref_vector const& vars, expr_ref_vector& conseq) {
        init_preprocess();
        TRACE("sat", tout << assumptions << "\n" << vars << "\n";);
        sat::literal_vector asms;
        sat::bool_var_vector bvars;
        vector<sat::literal_vector> lconseq;
        dep2asm_t dep2asm;
        m_solver.pop_to_base_level();
        lbool r = internalize_formulas();
        if (r != l_true) return r;
        r = internalize_vars(vars, bvars);
        if (r != l_true) return r;
        r = internalize_assumptions(assumptions.size(), assumptions.c_ptr(), dep2asm);
        if (r != l_true) return r;
        r = m_solver.get_consequences(m_asms, bvars, lconseq);
        if (r == l_false) {
            if (!m_asms.empty()) {
                extract_core(dep2asm);
            }
            return r;
        }

        // build map from bound variables to 
        // the consequences that cover them.
        u_map<unsigned> bool_var2conseq;
        for (unsigned i = 0; i < lconseq.size(); ++i) {
            TRACE("sat", tout << lconseq[i] << "\n";);
            bool_var2conseq.insert(lconseq[i][0].var(), i);
        }
        
        // extract original fixed variables
        u_map<expr*> asm2dep;
        extract_asm2dep(dep2asm, asm2dep);
        for (unsigned i = 0; i < vars.size(); ++i) {
            expr_ref cons(m);
            if (extract_fixed_variable(dep2asm, asm2dep, vars[i], bool_var2conseq, lconseq, cons)) {
                conseq.push_back(cons);
            }
        }

        return r;
    }

    virtual lbool find_mutexes(expr_ref_vector const& vars, vector<expr_ref_vector>& mutexes) {
        sat::literal_vector ls;
        u_map<expr*> lit2var;
        for (unsigned i = 0; i < vars.size(); ++i) {
            expr* e = vars[i];
            bool neg = m.is_not(e, e);
            sat::bool_var v = m_map.to_bool_var(e);
            if (v != sat::null_bool_var) {
                sat::literal lit(v, neg);
                ls.push_back(lit);
                lit2var.insert(lit.index(), vars[i]);
            }
        }
        vector<sat::literal_vector> ls_mutexes;
        m_solver.find_mutexes(ls, ls_mutexes);
        for (unsigned i = 0; i < ls_mutexes.size(); ++i) {
            sat::literal_vector const ls_mutex = ls_mutexes[i];
            expr_ref_vector mutex(m);
            for (unsigned j = 0; j < ls_mutex.size(); ++j) {
                mutex.push_back(lit2var.find(ls_mutex[j].index()));
            }
            mutexes.push_back(mutex);
        }
        return l_true;
    }


    virtual std::string reason_unknown() const {
        return m_unknown;
    }
    virtual void set_reason_unknown(char const* msg) {
        m_unknown = msg;
    }
    virtual void get_labels(svector<symbol> & r) {
    }
    virtual unsigned get_num_assertions() const {
        return m_fmls.size();
    }
    virtual expr * get_assertion(unsigned idx) const {
        return m_fmls[idx];
    }
    virtual unsigned get_num_assumptions() const {
        return m_asmsf.size();
    }
    virtual expr * get_assumption(unsigned idx) const {
        return m_asmsf[idx];
    }

    void init_preprocess() {
        if (m_preprocess) {
            m_preprocess->reset();
        }
        if (!m_bb_rewriter) {
            m_bb_rewriter = alloc(bit_blaster_rewriter, m, m_params);
        }
        params_ref simp2_p = m_params;
        simp2_p.set_bool("som", true);
        simp2_p.set_bool("pull_cheap_ite", true);
        simp2_p.set_bool("push_ite_bv", false);
        simp2_p.set_bool("local_ctx", true);
        simp2_p.set_uint("local_ctx_limit", 10000000);
        simp2_p.set_bool("flat", true); // required by som
        simp2_p.set_bool("hoist_mul", false); // required by som
        simp2_p.set_bool("elim_and", true);
        simp2_p.set_bool("blast_distinct", true);
        m_preprocess =
            and_then(mk_card2bv_tactic(m, m_params),
                     using_params(mk_simplify_tactic(m), simp2_p),
                     mk_max_bv_sharing_tactic(m),
                     mk_bit_blaster_tactic(m, m_bb_rewriter.get()),
                     //mk_aig_tactic(),
                     //mk_propagate_values_tactic(m, simp2_p),
                     using_params(mk_simplify_tactic(m), simp2_p));
        while (m_bb_rewriter->get_num_scopes() < m_num_scopes) {
            m_bb_rewriter->push();
        }
        m_preprocess->reset();
    }

private:


    lbool internalize_goal(goal_ref& g, dep2asm_t& dep2asm) {
        m_mc.reset();
        m_pc.reset();
        m_dep_core.reset();
        m_subgoals.reset();
        init_preprocess();
        SASSERT(g->models_enabled());
        SASSERT(!g->proofs_enabled());
        TRACE("sat", g->display(tout););
        try {
            (*m_preprocess)(g, m_subgoals, m_mc, m_pc, m_dep_core);
        }
        catch (tactic_exception & ex) {
            IF_VERBOSE(0, verbose_stream() << "exception in tactic " << ex.msg() << "\n";);
            TRACE("sat", tout << "exception: " << ex.msg() << "\n";);
            m_preprocess = 0;
            m_bb_rewriter = 0;
            return l_undef;
        }
        if (m_subgoals.size() != 1) {
            IF_VERBOSE(0, verbose_stream() << "size of subgoals is not 1, it is: " << m_subgoals.size() << "\n";);
            return l_undef;
        }
        g = m_subgoals[0];
        expr_ref_vector atoms(m);
        TRACE("sat", g->display_with_dependencies(tout););
        m_goal2sat(*g, m_params, m_solver, m_map, dep2asm, true);
        m_goal2sat.get_interpreted_atoms(atoms);
        if (!atoms.empty()) {
            std::stringstream strm;
            strm << "interpreted atoms sent to SAT solver " << atoms;
            TRACE("sat", tout << strm.str() << "\n";);
            IF_VERBOSE(1, verbose_stream() << strm.str() << "\n";);
            set_reason_unknown(strm.str().c_str());
            return l_undef;
        }
        return l_true;
    }

    lbool internalize_assumptions(unsigned sz, expr* const* asms, dep2asm_t& dep2asm) {
        if (sz == 0 && get_num_assumptions() == 0) {
            m_asms.shrink(0);
            return l_true;
        }
        goal_ref g = alloc(goal, m, true, true); // models and cores are enabled.
        for (unsigned i = 0; i < sz; ++i) {
            g->assert_expr(asms[i], m.mk_leaf(asms[i]));
        }
        for (unsigned i = 0; i < get_num_assumptions(); ++i) {
            g->assert_expr(get_assumption(i), m.mk_leaf(get_assumption(i)));
        }
        lbool res = internalize_goal(g, dep2asm);
        if (res == l_true) {
            extract_assumptions(sz, asms, dep2asm);
        }
        return res;
    }

    lbool internalize_vars(expr_ref_vector const& vars, sat::bool_var_vector& bvars) {
        for (unsigned i = 0; i < vars.size(); ++i) {
            internalize_var(vars[i], bvars);            
        }
        return l_true;
    }

    bool internalize_var(expr* v, sat::bool_var_vector& bvars) {
        obj_map<func_decl, expr*> const& const2bits = m_bb_rewriter->const2bits();
        expr* bv;
        bv_util bvutil(m);
        bool internalized = false;
        if (is_uninterp_const(v) && m.is_bool(v)) {
            sat::bool_var b = m_map.to_bool_var(v);
            
            if (b != sat::null_bool_var) {
                bvars.push_back(b);
                internalized = true;
            }
        }
        else if (is_uninterp_const(v) && const2bits.find(to_app(v)->get_decl(), bv)) {
            SASSERT(bvutil.is_bv(bv));
            app* abv = to_app(bv);
            internalized = true;
            unsigned sz = abv->get_num_args();
            for (unsigned j = 0; j < sz; ++j) {
                SASSERT(is_uninterp_const(abv->get_arg(j)));
                sat::bool_var b = m_map.to_bool_var(abv->get_arg(j));
                if (b == sat::null_bool_var) {
                    internalized = false;
                }
                else {
                    bvars.push_back(b);
                }
            }
            CTRACE("sat", internalized, tout << "var: "; for (unsigned j = 0; j < sz; ++j) tout << bvars[bvars.size()-sz+j] << " "; tout << "\n";);
        }
        else if (is_uninterp_const(v) && bvutil.is_bv(v)) {
            // variable does not occur in assertions, so is unconstrained.
        }
        CTRACE("sat", !internalized, tout << "unhandled variable " << mk_pp(v, m) << "\n";);        
        return internalized;
    }

    bool extract_fixed_variable(dep2asm_t& dep2asm, u_map<expr*>& asm2dep, expr* v, u_map<unsigned> const& bool_var2conseq, vector<sat::literal_vector> const& lconseq, expr_ref& conseq) {

        sat::bool_var_vector bvars;
        if (!internalize_var(v, bvars)) {
            return false;
        }
        sat::literal_vector value;
        sat::literal_set premises;
        for (unsigned i = 0; i < bvars.size(); ++i) {
            unsigned index;
            if (bool_var2conseq.find(bvars[i], index)) {
                value.push_back(lconseq[index][0]);
                for (unsigned j = 1; j < lconseq[index].size(); ++j) {
                    premises.insert(lconseq[index][j]);
                }
            }
            else {
                TRACE("sat", tout << "variable is not bound " << mk_pp(v, m) << "\n";);
                return false;
            }
        }
        expr_ref val(m);
        expr_ref_vector conj(m);
        internalize_value(value, v, val);        
        while (!premises.empty()) {
            expr* e = 0;
            VERIFY(asm2dep.find(premises.pop().index(), e));
            conj.push_back(e);
        }
        conseq = m.mk_implies(mk_and(conj), val);
        return true;
    }

    vector<rational> m_exps;
    void internalize_value(sat::literal_vector const& value, expr* v, expr_ref& val) {
        bv_util bvutil(m);
        if (is_uninterp_const(v) && m.is_bool(v)) {
            SASSERT(value.size() == 1);
            val = value[0].sign() ? m.mk_not(v) : v;
        }
        else if (is_uninterp_const(v) && bvutil.is_bv_sort(m.get_sort(v))) {
            SASSERT(value.size() == bvutil.get_bv_size(v));
            if (m_exps.empty()) {
                m_exps.push_back(rational::one());
            }
            while (m_exps.size() < value.size()) {
                m_exps.push_back(rational(2)*m_exps.back());
            }
            rational r(0);
            for (unsigned i = 0; i < value.size(); ++i) {
                if (!value[i].sign()) {
                    r += m_exps[i];
                }
            }
            val = m.mk_eq(v, bvutil.mk_numeral(r, value.size()));
        }
        else {
            UNREACHABLE();
        }
    }

    lbool internalize_formulas() {
        if (m_fmls_head == m_fmls.size()) {
            return l_true;
        }
        dep2asm_t dep2asm;
        goal_ref g = alloc(goal, m, true, false); // models, maybe cores are enabled
        for (unsigned i = m_fmls_head ; i < m_fmls.size(); ++i) {
            g->assert_expr(m_fmls[i].get());
        }
        lbool res = internalize_goal(g, dep2asm);
        if (res != l_undef) {
            m_fmls_head = m_fmls.size();
        }
        return res;
    }

    void extract_assumptions(unsigned sz, expr* const* asms, dep2asm_t& dep2asm) {
        m_asms.reset();
        unsigned j = 0;
        sat::literal lit;
        for (unsigned i = 0; i < sz; ++i) {
            if (dep2asm.find(asms[i], lit)) {
                SASSERT(lit.var() <= m_solver.num_vars());
                m_asms.push_back(lit);
                if (i != j && !m_weights.empty()) {
                    m_weights[j] = m_weights[i];
                }
                ++j;
            }
        }
        for (unsigned i = 0; i < get_num_assumptions(); ++i) {
            if (dep2asm.find(get_assumption(i), lit)) {
                SASSERT(lit.var() <= m_solver.num_vars());
                m_asms.push_back(lit);
            }
        }

        SASSERT(dep2asm.size() == m_asms.size());
    }

    void extract_asm2dep(dep2asm_t const& dep2asm, u_map<expr*>& asm2dep) {
        dep2asm_t::iterator it = dep2asm.begin(), end = dep2asm.end();
        for (; it != end; ++it) {
            expr* e = it->m_key;
            asm2dep.insert(it->m_value.index(), e);
        }
    }

    void extract_core(dep2asm_t& dep2asm) {
        u_map<expr*> asm2dep;
        extract_asm2dep(dep2asm, asm2dep);
        sat::literal_vector const& core = m_solver.get_core();
        TRACE("sat",
              dep2asm_t::iterator it2 = dep2asm.begin();
              dep2asm_t::iterator end2 = dep2asm.end();
              for (; it2 != end2; ++it2) {
                  tout << mk_pp(it2->m_key, m) << " |-> " << sat::literal(it2->m_value) << "\n";
              }
              tout << "core: ";
              for (unsigned i = 0; i < core.size(); ++i) {
                  tout << core[i] << " ";
              }
              tout << "\n";
              );

        m_core.reset();
        for (unsigned i = 0; i < core.size(); ++i) {
            expr* e = 0;
            VERIFY(asm2dep.find(core[i].index(), e));
            m_core.push_back(e);
        }
    }

    void check_assumptions(dep2asm_t& dep2asm) {
        sat::model const & ll_m = m_solver.get_model();
        dep2asm_t::iterator it = dep2asm.begin(), end = dep2asm.end();
        for (; it != end; ++it) {
            sat::literal lit = it->m_value;
            if (sat::value_at(lit, ll_m) != l_true) {
                IF_VERBOSE(0, verbose_stream() << mk_pp(it->m_key, m) << " does not evaluate to true\n";
                           verbose_stream() << m_asms << "\n";
                           m_solver.display_assignment(verbose_stream());
                           m_solver.display(verbose_stream()););
                throw default_exception("bad state");
            }
        }
    }

    void extract_model() {
        TRACE("sat", tout << "retrieve model " << (m_solver.model_is_current()?"present":"absent") << "\n";);
        if (!m_solver.model_is_current()) {
            m_model = 0;
            return;
        }
        sat::model const & ll_m = m_solver.get_model();
        model_ref md = alloc(model, m);
        atom2bool_var::iterator it  = m_map.begin();
        atom2bool_var::iterator end = m_map.end();
        for (; it != end; ++it) {
            expr * n   = it->m_key;
            if (is_app(n) && to_app(n)->get_num_args() > 0) {
                continue;
            }
            sat::bool_var v = it->m_value;
            switch (sat::value_at(v, ll_m)) {
            case l_true:
                md->register_decl(to_app(n)->get_decl(), m.mk_true());
                break;
            case l_false:
                md->register_decl(to_app(n)->get_decl(), m.mk_false());
                break;
            default:
                break;
            }
        }
        m_model = md;

        if (m_bb_rewriter.get() && !m_bb_rewriter->const2bits().empty()) {
            m_mc0 = concat(m_mc0.get(), mk_bit_blaster_model_converter(m, m_bb_rewriter->const2bits()));
        }
        if (m_mc0) {
            (*m_mc0)(m_model);
        }
        SASSERT(m_model);

        DEBUG_CODE(
            for (unsigned i = 0; i < m_fmls.size(); ++i) {
                expr_ref tmp(m);
                if (m_model->eval(m_fmls[i].get(), tmp, true)) {
                    CTRACE("sat", !m.is_true(tmp),
                           tout << "Evaluation failed: " << mk_pp(m_fmls[i].get(), m)
                           << " to " << tmp << "\n";
                           model_smt2_pp(tout, m, *(m_model.get()), 0););
                    SASSERT(m.is_true(tmp));
                }
            });
    }
};


solver* mk_inc_sat_solver(ast_manager& m, params_ref const& p) {
    return alloc(inc_sat_solver, m, p);
}


lbool inc_sat_check_sat(solver& _s, unsigned sz, expr*const* soft, rational const* _weights, rational const& max_weight) {
    inc_sat_solver& s = dynamic_cast<inc_sat_solver&>(_s);
    vector<double> weights;
    for (unsigned i = 0; _weights && i < sz; ++i) {
        weights.push_back(_weights[i].get_double());
    }
    params_ref p;
    p.set_bool("minimize_core", false);
    s.updt_params(p);
    return s.check_sat(sz, soft, weights.c_ptr(), max_weight.get_double());
}

void inc_sat_display(std::ostream& out, solver& _s, unsigned sz, expr*const* soft, rational const* _weights) {
    inc_sat_solver& s = dynamic_cast<inc_sat_solver&>(_s);
    vector<unsigned> weights;
    for (unsigned i = 0; _weights && i < sz; ++i) {
        if (!_weights[i].is_unsigned()) {
            throw default_exception("Cannot display weights that are not integers");
        }
        weights.push_back(_weights[i].get_unsigned());
    }
    s.display_weighted(out, sz, soft, weights.c_ptr());
}

