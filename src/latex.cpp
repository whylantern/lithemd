#include "latex.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

using Microsoft::WRL::ComPtr;

namespace {

const std::map<std::wstring, std::wstring>& Symbols() {
    static const std::map<std::wstring, std::wstring> m = {
        { L"alpha", L"α" },{ L"beta", L"β" },{ L"gamma", L"γ" },{ L"delta", L"δ" },
        { L"epsilon", L"ε" },{ L"varepsilon", L"ε" },{ L"zeta", L"ζ" },{ L"eta", L"η" },
        { L"theta", L"θ" },{ L"vartheta", L"ϑ" },{ L"iota", L"ι" },{ L"kappa", L"κ" },
        { L"lambda", L"λ" },{ L"mu", L"μ" },{ L"nu", L"ν" },{ L"xi", L"ξ" },
        { L"omicron", L"ο" },{ L"pi", L"π" },{ L"varpi", L"ϖ" },{ L"rho", L"ρ" },
        { L"varrho", L"ϱ" },{ L"sigma", L"σ" },{ L"varsigma", L"ς" },{ L"tau", L"τ" },
        { L"upsilon", L"υ" },{ L"phi", L"φ" },{ L"varphi", L"ϕ" },{ L"chi", L"χ" },
        { L"psi", L"ψ" },{ L"omega", L"ω" },
        { L"Gamma", L"Γ" },{ L"Delta", L"Δ" },{ L"Theta", L"Θ" },{ L"Lambda", L"Λ" },
        { L"Xi", L"Ξ" },{ L"Pi", L"Π" },{ L"Sigma", L"Σ" },{ L"Upsilon", L"Υ" },
        { L"Phi", L"Φ" },{ L"Psi", L"Ψ" },{ L"Omega", L"Ω" },
        { L"cdot", L"·" },{ L"times", L"×" },{ L"div", L"÷" },{ L"pm", L"±" },
        { L"mp", L"∓" },{ L"ast", L"∗" },{ L"star", L"⋆" },{ L"circ", L"∘" },
        { L"bullet", L"•" },{ L"oplus", L"⊕" },{ L"otimes", L"⊗" },{ L"odot", L"⊙" },
        { L"le", L"≤" },{ L"leq", L"≤" },{ L"ge", L"≥" },{ L"geq", L"≥" },
        { L"ne", L"≠" },{ L"neq", L"≠" },{ L"approx", L"≈" },{ L"equiv", L"≡" },
        { L"sim", L"∼" },{ L"simeq", L"≃" },{ L"cong", L"≅" },{ L"propto", L"∝" },
        { L"ll", L"≪" },{ L"gg", L"≫" },{ L"in", L"∈" },{ L"notin", L"∉" },
        { L"ni", L"∋" },{ L"subset", L"⊂" },{ L"subseteq", L"⊆" },{ L"supset", L"⊃" },
        { L"supseteq", L"⊇" },{ L"cup", L"∪" },{ L"cap", L"∩" },{ L"setminus", L"∖" },
        { L"mid", L"∣" },{ L"nmid", L"∤" },
        { L"lfloor", L"⌊" },{ L"rfloor", L"⌋" },
        { L"lceil", L"⌈" },{ L"rceil", L"⌉" },
        { L"bigcup", L"⋃" },{ L"bigcap", L"⋂" },{ L"bigvee", L"⋁" },{ L"bigwedge", L"⋀" },
        { L"bigoplus", L"⨁" },{ L"bigotimes", L"⨂" },{ L"bigodot", L"⨀" },
        { L"emptyset", L"∅" },{ L"varnothing", L"∅" },{ L"forall", L"∀" },{ L"exists", L"∃" },
        { L"neg", L"¬" },{ L"land", L"∧" },{ L"lor", L"∨" },{ L"wedge", L"∧" },{ L"vee", L"∨" },
        { L"to", L"→" },{ L"rightarrow", L"→" },{ L"leftarrow", L"←" },{ L"Rightarrow", L"⇒" },
        { L"Leftarrow", L"⇐" },{ L"Leftrightarrow", L"⇔" },{ L"leftrightarrow", L"↔" },
        { L"implies", L"⟹" },{ L"impliedby", L"⟸" },{ L"iff", L"⟺" },{ L"mapsto", L"↦" },
        { L"uparrow", L"↑" },{ L"downarrow", L"↓" },{ L"longrightarrow", L"⟶" },
        { L"infty", L"∞" },{ L"partial", L"∂" },{ L"nabla", L"∇" },{ L"sum", L"∑" },
        { L"prod", L"∏" },{ L"coprod", L"∐" },{ L"int", L"∫" },{ L"iint", L"∬" },
        { L"iiint", L"∭" },{ L"oint", L"∮" },{ L"ell", L"ℓ" },{ L"hbar", L"ℏ" },
        { L"Re", L"ℜ" },{ L"Im", L"ℑ" },{ L"aleph", L"ℵ" },{ L"angle", L"∠" },
        { L"perp", L"⊥" },{ L"parallel", L"∥" },{ L"degree", L"°" },{ L"prime", L"′" },
        { L"therefore", L"∴" },{ L"because", L"∵" },{ L"dots", L"…" },{ L"ldots", L"…" },
        { L"cdots", L"⋯" },{ L"vdots", L"⋮" },{ L"ddots", L"⋱" },
        { L"sin", L"sin" },{ L"cos", L"cos" },{ L"tan", L"tan" },{ L"cot", L"cot" },
        { L"sec", L"sec" },{ L"csc", L"csc" },{ L"arcsin", L"arcsin" },{ L"arccos", L"arccos" },
        { L"arctan", L"arctan" },{ L"sinh", L"sinh" },{ L"cosh", L"cosh" },{ L"tanh", L"tanh" },
        { L"log", L"log" },{ L"ln", L"ln" },{ L"exp", L"exp" },{ L"lim", L"lim" },
        { L"max", L"max" },{ L"min", L"min" },{ L"sup", L"sup" },{ L"inf", L"inf" },
        { L"det", L"det" },{ L"dim", L"dim" },{ L"ker", L"ker" },{ L"deg", L"deg" },
        { L"gcd", L"gcd" },{ L"mod", L"mod" },{ L"bmod", L"mod" }
    };
    return m;
}

std::shared_ptr<MathNode> MakeText(const std::wstring& t, bool italic = false, bool bold = false, bool roman = false) {
    auto n = std::make_shared<MathNode>();
    n->kind = MathKind::Text;
    n->text = t;
    n->italic = italic;
    n->bold = bold;
    n->roman = roman;
    return n;
}
std::shared_ptr<MathNode> MakeSpace(float em) {
    auto n = std::make_shared<MathNode>();
    n->kind = MathKind::Space;
    n->em = em;
    return n;
}
std::shared_ptr<MathNode> MakeRow(std::vector<std::shared_ptr<MathNode>> items) {
    if (items.empty()) return nullptr;
    if (items.size() == 1) return items[0];
    auto n = std::make_shared<MathNode>();
    n->kind = MathKind::Row;
    n->items = std::move(items);
    return n;
}

constexpr int kMaxMathDepth = 128;

struct MathParser {
    const std::wstring& s;
    size_t p = 0;
    bool display = false;
    int depth_ = 0;

    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    };

    MathParser(const std::wstring& text, bool disp) : s(text), display(disp) {}
    bool Eof() const { return p >= s.size(); }
    wchar_t Peek(size_t n = 0) const { return p + n < s.size() ? s[p + n] : 0; }
    void SkipSpace() { while (!Eof() && iswspace(Peek())) p++; }

    std::wstring ReadCommand() {
        if (Peek() != L'\\') return L"";
        p++;
        if (Eof()) return L"";
        if (!iswalpha(Peek())) {
            wchar_t c = Peek();
            p++;
            return std::wstring(1, c);
        }
        size_t start = p;
        while (!Eof() && iswalpha(Peek())) p++;
        return s.substr(start, p - start);
    }

    std::wstring ReadRawGroup() {
        SkipSpace();
        if (Peek() != L'{') {
            if (Eof()) return L"";
            return std::wstring(1, s[p++]);
        }
        p++;
        int depth = 1;
        std::wstring out;
        while (!Eof() && depth > 0) {
            wchar_t c = s[p++];
            if (c == L'{') { depth++; if (depth > 1) out += c; }
            else if (c == L'}') { depth--; if (depth > 0) out += c; }
            else out += c;
        }
        return out;
    }

    std::shared_ptr<MathNode> ParseArg() {
        SkipSpace();
        if (Peek() == L'{') {
            p++;
            auto n = ParseRow();
            if (Peek() == L'}') p++;
            return n;
        }
        if (Eof()) return nullptr;
        return ParseAtom(false);
    }

    std::shared_ptr<MathNode> ParseRow() {
        std::vector<std::shared_ptr<MathNode>> items;
        while (!Eof()) {
            wchar_t c = Peek();
            if (c == L'}') break;
            if (c == L'&') { p++; continue; }
            if (c == L'\\' && Peek(1) == L'\\') { p += 2; items.push_back(MakeSpace(0.5f)); continue; }
            size_t before = p;
            auto atom = ParseAtom(true);
            if (!atom) {
                // Some control words (\\limits, \\displaystyle, ...) are
                // intentionally ignored and return nullptr after consuming
                // themselves. Only skip a character when the parser made no
                // progress, otherwise the next "_" or "^" would be lost.
                if (p == before && !Eof() && Peek() != L'}') p++;
                continue;
            }
            // scripts
            bool forceLimits = false;
            bool suppressLimits = false;
            for (;;) {
                SkipSpace();
                // \limits / \nolimits are modifiers of the preceding atom,
                // not atoms themselves. Consume them before looking for ^/_.
                while (Peek() == L'\\') {
                    size_t save = p;
                    std::wstring mod = ReadCommand();
                    if (mod == L"limits") { forceLimits = true; suppressLimits = false; }
                    else if (mod == L"nolimits") { suppressLimits = true; forceLimits = false; }
                    else { p = save; break; }
                    SkipSpace();
                }
                if (Peek() != L'^' && Peek() != L'_' && Peek() != L'\'') break;
                if (Peek() == L'\'') {
                    p++;
                    int count = 1;
                    while (Peek() == L'\'') { p++; count++; }
                    std::wstring primes;
                    for (int i = 0; i < count; i++) primes += L"′";
                    auto sup = MakeText(primes);
                    auto script = std::make_shared<MathNode>();
                    script->kind = MathKind::Script;
                    script->a = atom;
                    script->b = sup;
                    atom = script;
                    continue;
                }
                bool sup = Peek() == L'^';
                p++;
                auto arg = ParseArg();
                if (!arg) continue;
                std::shared_ptr<MathNode> script;
                if (atom->kind == MathKind::Script) script = atom;
                else {
                    script = std::make_shared<MathNode>();
                    script->kind = MathKind::Script;
                    script->a = atom;
                }
                bool limitOp = atom->kind == MathKind::Text &&
                    (atom->text == L"∑" || atom->text == L"∏" || atom->text == L"∫" ||
                     atom->text == L"∬" || atom->text == L"∭" || atom->text == L"∮" ||
                     atom->text == L"⋂" || atom->text == L"⋃" || atom->text == L"⋁" ||
                     atom->text == L"⋀" || atom->text == L"⨁" || atom->text == L"⨂" ||
                     atom->text == L"⨀" ||
                     atom->text == L"lim" || atom->text == L"max" || atom->text == L"min");
                if ((forceLimits || (display && limitOp)) && !suppressLimits)
                    script->displayLimits = true;
                else if (suppressLimits)
                    script->displayLimits = false;
                if (sup) script->b = arg; else script->c = arg;
                atom = script;
            }
            if (atom && atom->kind == MathKind::Script) {
                if (suppressLimits) atom->displayLimits = false;
                else if (forceLimits) atom->displayLimits = true;
            }
            items.push_back(atom);
        }
        // TeX inserts thin spaces around binary operators and relations. This
        // keeps formulas such as "x ≡ 1 (mod m)" readable without making the
        // parser context-sensitive.
        static const std::set<std::wstring> ops = {
            L"=", L"+", L"-", L"×", L"÷", L"±", L"∓", L"·", L"≤", L"≥", L"≠",
            L"≈", L"≡", L"∼", L"≃", L"≅", L"∝", L"→", L"←", L"⇒", L"⇐", L"⇔",
            L"↔", L"⟹", L"⟸", L"⟺", L"↦", L"∈", L"∉", L"∋", L"⊂", L"⊆",
            L"⊃", L"⊇", L"∪", L"∩", L"∖", L"<", L">", L"∀", L"∃", L"∣", L"∤"
        };
        if (items.size() >= 2) {
            std::vector<std::shared_ptr<MathNode>> spaced;
            for (size_t k = 0; k < items.size(); k++) {
                auto& it = items[k];
                bool op = it && it->kind == MathKind::Text && ops.count(it->text);
                bool leftSpace = k > 0 && items[k - 1] && items[k - 1]->kind == MathKind::Space;
                bool rightSpace = k + 1 < items.size() && items[k + 1] && items[k + 1]->kind == MathKind::Space;
                if (op && k > 0 && k + 1 < items.size() && !leftSpace) spaced.push_back(MakeSpace(0.14f));
                spaced.push_back(it);
                if (op && k > 0 && k + 1 < items.size() && !rightSpace) spaced.push_back(MakeSpace(0.14f));
            }
            items = std::move(spaced);
        }
        // TeX-style punctuation spacing. Without it an italic letter's
        // overhang can visually collide with the following comma, and the
        // source space after "," is ignored by math mode.
        if (items.size() >= 2) {
            std::vector<std::shared_ptr<MathNode>> spaced;
            for (size_t k = 0; k < items.size(); k++) {
                auto& it = items[k];
                bool punct = it && it->kind == MathKind::Text &&
                             (it->text == L"," || it->text == L";");
                bool closeDelim = it && it->kind == MathKind::Text &&
                                  (it->text == L"]" || it->text == L")" || it->text == L"}");
                bool closeBar = it && it->kind == MathKind::Text && it->text == L"|";
                bool leftSpace = k > 0 && items[k - 1] && items[k - 1]->kind == MathKind::Space;
                bool rightSpace = k + 1 < items.size() && items[k + 1] && items[k + 1]->kind == MathKind::Space;
                bool emptyPair = k > 0 && items[k - 1] && items[k - 1]->kind == MathKind::Text &&
                                 (items[k - 1]->text == L"[" || items[k - 1]->text == L"(" ||
                                  items[k - 1]->text == L"{" || items[k - 1]->text == L"|");
                bool prevOperator = k > 0 && items[k - 1] && items[k - 1]->kind == MathKind::Text &&
                                    ops.count(items[k - 1]->text) != 0;
                if (punct && k > 0 && !leftSpace) spaced.push_back(MakeSpace(0.035f));
                if ((closeDelim || (closeBar && !prevOperator)) && k > 0 && !leftSpace && !emptyPair)
                    spaced.push_back(MakeSpace(0.08f));
                spaced.push_back(it);
                if (punct && k + 1 < items.size() && !rightSpace) spaced.push_back(MakeSpace(0.10f));
            }
            items = std::move(spaced);
        }
        return MakeRow(std::move(items));
    }

    std::shared_ptr<MathNode> ParseDelimited(const std::wstring& inner, wchar_t open, wchar_t close) {
        SkipSpace();
        std::wstring content = inner;
        auto n = std::make_shared<MathNode>();
        n->kind = MathKind::Delim;
        n->openDelim = open;
        n->closeDelim = close;
        MathParser sub(content, display);
        sub.depth_ = depth_ + 1;
        n->a = sub.ParseRow();
        if (!n->a) n->a = MakeText(L"");
        return n;
    }

    std::shared_ptr<MathNode> ParseAtom(bool allowScripts) {
        (void)allowScripts;
        SkipSpace();
        if (Eof()) return nullptr;
        DepthGuard guard(depth_);
        if (depth_ > kMaxMathDepth) {
            // Do not recurse further on pathological formulas; consume the
            // rest of a command/group so the caller always makes progress.
            if (Peek() == L'\\') {
                ReadCommand();
                if (Peek() == L'{') ReadRawGroup();
            } else if (Peek() == L'{') {
                ReadRawGroup();
            } else {
                p++;
            }
            return MakeText(L"…");
        }
        wchar_t c = Peek();
        if (c == L'{') {
            p++;
            auto n = ParseRow();
            if (Peek() == L'}') p++;
            return n;
        }
        if (c == L'\\') {
            size_t cmdStart = p;
            std::wstring cmd = ReadCommand();
            if (cmd.empty()) return nullptr;
            if (cmd == L"frac" || cmd == L"dfrac" || cmd == L"tfrac" || cmd == L"cfrac") {
                auto num = ParseArg();
                auto den = ParseArg();
                auto n = std::make_shared<MathNode>();
                n->kind = MathKind::Fraction;
                n->a = num ? num : MakeText(L"");
                n->b = den ? den : MakeText(L"");
                return n;
            }
            if (cmd == L"binom" || cmd == L"dbinom" || cmd == L"tbinom") {
                auto top = ParseArg();
                auto bottom = ParseArg();
                auto frac = std::make_shared<MathNode>();
                frac->kind = MathKind::Fraction;
                frac->a = top ? top : MakeText(L"");
                frac->b = bottom ? bottom : MakeText(L"");
                auto d = std::make_shared<MathNode>();
                d->kind = MathKind::Delim;
                d->openDelim = L'(';
                d->closeDelim = L')';
                d->a = frac;
                return d;
            }
            if (cmd == L"overset" || cmd == L"stackrel" || cmd == L"underset") {
                auto scripted = ParseArg();
                auto base = ParseArg();
                auto n = std::make_shared<MathNode>();
                n->kind = MathKind::Script;
                n->a = base ? base : MakeText(L"");
                if (cmd == L"underset") n->c = scripted;
                else n->b = scripted;
                n->displayLimits = true;
                return n;
            }
            if (cmd == L"substack") {
                std::wstring raw = ReadRawGroup();
                auto n = std::make_shared<MathNode>();
                n->kind = MathKind::Matrix;
                std::vector<std::shared_ptr<MathNode>> col;
                std::wstring cur;
                for (size_t k = 0; k <= raw.size(); k++) {
                    wchar_t ch = k < raw.size() ? raw[k] : 0;
                    if (k == raw.size() || (ch == L'\\' && k + 1 < raw.size() && raw[k + 1] == L'\\')) {
                        MathParser sub(cur, false);
                        sub.depth_ = depth_ + 1;
                        auto row = sub.ParseRow();
                        if (row) col.push_back(row);
                        cur.clear();
                        if (ch == L'\\') k++;
                    } else cur += ch;
                }
                for (auto& r : col) n->cells.push_back({ r });
                return n;
            }
            if (cmd == L"sqrt") {
                SkipSpace();
                std::shared_ptr<MathNode> index;
                if (Peek() == L'[') {
                    p++;
                    int depth = 1;
                    std::wstring t;
                    while (!Eof() && depth > 0) {
                        wchar_t x = s[p++];
                        if (x == L'[') { depth++; if (depth > 1) t += x; }
                        else if (x == L']') { depth--; if (depth > 0) t += x; }
                        else t += x;
                    }
                    MathParser sub(t, display);
                    sub.depth_ = depth_ + 1;
                    index = sub.ParseRow();
                }
                auto body = ParseArg();
                auto n = std::make_shared<MathNode>();
                n->kind = MathKind::Sqrt;
                n->a = body ? body : MakeText(L"");
                n->b = index;
                return n;
            }
            if (cmd == L"text" || cmd == L"textrm" || cmd == L"textnormal" || cmd == L"mbox" ||
                cmd == L"operatorname" || cmd == L"mathrm" || cmd == L"mathit" || cmd == L"mathbf" ||
                cmd == L"textbf" || cmd == L"mathsf" || cmd == L"mathtt" || cmd == L"mathbb" ||
                cmd == L"mathcal" || cmd == L"emph") {
                std::wstring raw = ReadRawGroup();
                if (cmd == L"mathbb") {
                    std::wstring mapped;
                    for (wchar_t ch : raw) {
                        if (ch == L'R') mapped += L"ℝ";
                        else if (ch == L'N') mapped += L"ℕ";
                        else if (ch == L'Z') mapped += L"ℤ";
                        else if (ch == L'Q') mapped += L"ℚ";
                        else if (ch == L'C') mapped += L"ℂ";
                        else mapped += ch;
                    }
                    raw = mapped;
                }
                return MakeText(raw, cmd == L"mathit" || cmd == L"emph",
                                cmd == L"mathbf" || cmd == L"textbf", true);
            }
            if (cmd == L"overline" || cmd == L"underline" || cmd == L"vec" || cmd == L"hat" ||
                cmd == L"bar" || cmd == L"dot" || cmd == L"ddot" || cmd == L"tilde" ||
                cmd == L"widehat" || cmd == L"widetilde" || cmd == L"overrightarrow") {
                auto body = ParseArg();
                auto n = std::make_shared<MathNode>();
                n->kind = MathKind::Accent;
                n->a = body ? body : MakeText(L"");
                if (cmd == L"overline" || cmd == L"bar" || cmd == L"overrightarrow") n->accent = MathAccent::Overline;
                else if (cmd == L"underline") n->accent = MathAccent::Underline;
                else if (cmd == L"vec") n->accent = MathAccent::Vec;
                else if (cmd == L"hat" || cmd == L"widehat") n->accent = MathAccent::Hat;
                else if (cmd == L"dot") n->accent = MathAccent::Dot;
                else if (cmd == L"ddot") n->accent = MathAccent::Ddot;
                else n->accent = MathAccent::Tilde;
                return n;
            }
            if (cmd == L"left") {
                SkipSpace();
                wchar_t open = 0;
                if (Peek() == L'\\') {
                    std::wstring d = ReadCommand();
                    if (d == L"{") open = L'{';
                    else if (d == L"}") open = L'}';
                    else if (d == L"|") open = L'‖';
                    else if (d == L"lfloor") open = L'⌊';
                    else if (d == L"rfloor") open = L'⌋';
                    else if (d == L"lceil") open = L'⌈';
                    else if (d == L"rceil") open = L'⌉';
                    else if (d == L".") open = 0;
                    else if (!d.empty()) open = d[0];
                } else if (!Eof()) open = s[p++];
                // find matching \right at this level
                int depth = 1;
                size_t q = p;
                size_t rightPos = std::wstring::npos;
                while (q < s.size()) {
                    if (s.compare(q, 5, L"\\left") == 0) { depth++; q += 5; continue; }
                    if (s.compare(q, 6, L"\\right") == 0) {
                        depth--;
                        if (depth == 0) { rightPos = q; break; }
                        q += 6;
                        continue;
                    }
                    q++;
                }
                std::wstring inner = rightPos == std::wstring::npos ? s.substr(p) : s.substr(p, rightPos - p);
                wchar_t close = 0;
                if (rightPos != std::wstring::npos) {
                    p = rightPos + 6;
                    SkipSpace();
                    if (Peek() == L'\\') {
                        std::wstring d = ReadCommand();
                        if (d == L"}" || d == L"{") close = d[0];
                        else if (d == L"|") close = L'‖';
                        else if (d == L"lfloor") close = L'⌊';
                        else if (d == L"rfloor") close = L'⌋';
                        else if (d == L"lceil") close = L'⌈';
                        else if (d == L"rceil") close = L'⌉';
                        else if (d == L".") close = 0;
                        else if (!d.empty()) close = d[0];
                    } else if (!Eof()) close = s[p++];
                } else p = s.size();
                return ParseDelimited(inner, open, close);
            }
            if (cmd == L"begin") {
                std::wstring env = ReadRawGroup();
                std::wstring endTag = L"\\end{" + env + L"}";
                size_t endPos = s.find(endTag, p);
                std::wstring body = endPos == std::wstring::npos ? s.substr(p) : s.substr(p, endPos - p);
                if (endPos != std::wstring::npos) p = endPos + endTag.size();
                else p = s.size();
                if (env == L"array" || env == L"aligned" || env == L"align" || env == L"align*") {
                    // drop column specification
                    if (!body.empty() && body[0] == L'{') {
                        int depth = 0;
                        size_t k = 0;
                        for (; k < body.size(); k++) {
                            if (body[k] == L'{') depth++;
                            else if (body[k] == L'}') { depth--; if (depth == 0) { k++; break; } }
                        }
                        body = body.substr(k);
                    }
                }
                auto n = std::make_shared<MathNode>();
                n->kind = MathKind::Matrix;
                if (env == L"pmatrix") { n->openDelim = L'('; n->closeDelim = L')'; }
                else if (env == L"bmatrix") { n->openDelim = L'['; n->closeDelim = L']'; }
                else if (env == L"Bmatrix") { n->openDelim = L'{'; n->closeDelim = L'}'; }
                else if (env == L"vmatrix") { n->openDelim = L'|'; n->closeDelim = L'|'; }
                else if (env == L"Vmatrix") { n->openDelim = L'‖'; n->closeDelim = L'‖'; }
                else if (env == L"cases") { n->openDelim = L'{'; n->closeDelim = 0; }
                // split rows / cells at top level
                std::vector<std::wstring> rows;
                std::wstring cur;
                int depth = 0;
                for (size_t k = 0; k < body.size(); k++) {
                    wchar_t ch = body[k];
                    if (ch == L'{') { depth++; cur += ch; }
                    else if (ch == L'}') { if (depth > 0) depth--; cur += ch; }
                    else if (depth == 0 && ch == L'\\' && k + 1 < body.size() && body[k + 1] == L'\\') {
                        rows.push_back(cur); cur.clear(); k++;
                    } else cur += ch;
                }
                if (!Trim(cur).empty() || !rows.empty()) rows.push_back(cur);
                for (auto& rowText : rows) {
                    std::vector<std::shared_ptr<MathNode>> cells;
                    std::wstring cell;
                    depth = 0;
                    for (size_t k = 0; k <= rowText.size(); k++) {
                        wchar_t ch = k < rowText.size() ? rowText[k] : 0;
                        if (ch == L'{') depth++;
                        else if (ch == L'}') { if (depth > 0) depth--; }
                        if (k == rowText.size() || (ch == L'&' && depth == 0)) {
                            MathParser sub(cell, display);
                            sub.depth_ = depth_ + 1;
                            auto parsed = sub.ParseRow();
                            cells.push_back(parsed ? parsed : MakeText(L""));
                            cell.clear();
                        } else cell += ch;
                    }
                    if (!cells.empty()) n->cells.push_back(std::move(cells));
                }
                return n;
            }
            if (cmd == L"bmod") {
                return MakeRow({ MakeSpace(0.28f), MakeText(L"mod", false, false, true), MakeSpace(0.28f) });
            }
            if (cmd == L"pmod") {
                auto body = ParseArg();
                std::wstring inner = body ? MathPlainText(*body) : L"";
                return MakeText(L" (mod " + inner + L")", false, false, true);
            }
            if (cmd == L"tag" || cmd == L"label" || cmd == L"eqref" || cmd == L"ref") {
                ReadRawGroup();
                return nullptr;
            }
            if (cmd == L"operatorname" || cmd == L"mathop") {
                if (Peek() == L'*') p++;
                return MakeText(ReadRawGroup(), false, false, true);
            }
            if (cmd == L"quad") return MakeSpace(1.0f);
            if (cmd == L"qquad") return MakeSpace(2.0f);
            if (cmd == L",") return MakeSpace(0.17f);
            if (cmd == L":") return MakeSpace(0.22f);
            if (cmd == L";") return MakeSpace(0.28f);
            if (cmd == L"!") return MakeSpace(0.0f);
            if (cmd == L" ") return MakeSpace(0.33f);
            if (cmd == L"\\") return MakeSpace(0.5f);
            if (cmd == L"{" || cmd == L"}" || cmd == L"$" || cmd == L"&" || cmd == L"%") return MakeText(cmd);
            if (cmd == L"|" || cmd == L"Vert") return MakeText(L"‖");
            if (cmd == L"vert") return MakeText(L"|");
            if (cmd == L"lvert" || cmd == L"lVert" || cmd == L"rvert" || cmd == L"rVert")
                return MakeText(cmd == L"lvert" || cmd == L"rvert" ? L"|" : L"‖");
            if (cmd == L"not") {
                auto next = ParseArg();
                std::wstring t = next ? MathPlainText(*next) : L"";
                std::wstring neg;
                if (t == L"∣") neg = L"∤";
                else if (t == L"≡") neg = L"≢";
                else if (t == L"∈") neg = L"∉";
                else if (t == L"⊂") neg = L"⊄";
                else if (t == L"⊆") neg = L"⊈";
                else if (t == L"⊃") neg = L"⊅";
                else if (t == L"⊇") neg = L"⊉";
                else if (t == L"≈") neg = L"≉";
                else if (t == L"∼") neg = L"≁";
                else if (t == L"∥") neg = L"∦";
                else if (t == L"=") neg = L"≠";
                else if (t == L"<") neg = L"≮";
                else if (t == L">") neg = L"≯";
                if (!neg.empty()) return MakeText(neg);
                return MakeRow({ MakeText(L"¬"), next ? next : MakeText(L"") });
            }
            if (cmd == L"boxed" || cmd == L"underbrace" || cmd == L"overbrace" ||
                cmd == L"vphantom" || cmd == L"phantom" || cmd == L"hphantom" || cmd == L"smash") {
                auto body = ParseArg();
                return body ? body : MakeText(L"");
            }
            if (cmd == L"color") { ReadRawGroup(); return nullptr; }
            if (cmd == L"textcolor") { ReadRawGroup(); return ParseArg(); }
            static const std::set<std::wstring> skip = {
                L"displaystyle", L"textstyle", L"scriptstyle", L"scriptscriptstyle", L"limits",
                L"nolimits", L"big", L"Big", L"bigg", L"Bigg", L"bigl", L"bigr", L"Bigl",
                L"Bigr", L"biggl", L"biggr", L"Biggl", L"Biggr", L"middle", L"nonumber",
                L"notag", L"right", L"end", L"color", L"textcolor", L"vphantom", L"phantom",
                L"hphantom", L"smash", L"mathstrut", L"allowbreak", L"nobreak"
            };
            if (skip.count(cmd)) return nullptr;
            auto it = Symbols().find(cmd);
            if (it != Symbols().end()) {
                bool roman = (cmd == L"sin" || cmd == L"cos" || cmd == L"tan" || cmd == L"lim" ||
                              cmd == L"log" || cmd == L"ln" || cmd == L"max" || cmd == L"min" ||
                              cmd == L"det" || cmd == L"inf" || cmd == L"sup" || cmd == L"mod");
                return MakeText(it->second, false, false, roman);
            }
            (void)cmdStart;
            // unknown command: show its name
            return MakeText(cmd, false, false, true);
        }
        if (c == L'^' || c == L'_') return nullptr; // handled by caller
        p++;
        if (c == L'~') return MakeSpace(0.33f);
        if (c == L' ') return nullptr;
        if (iswdigit(c) || wcschr(L"+-=<>()[]|/.,:;!?", c)) return MakeText(std::wstring(1, c));
        if (iswalpha(c) || c > 0x7F) return MakeText(std::wstring(1, c), true, false, false);
        return MakeText(std::wstring(1, c));
    }
};

ComPtr<IDWriteTextLayout> MakeTextLayout(IDWriteFactory* dwrite, const std::wstring& text, float size,
                                        bool italic, bool bold, bool roman, DWRITE_TEXT_METRICS* metrics,
                                        FLOAT* baseline) {
    ComPtr<IDWriteTextFormat> fmt;
    const wchar_t* family = roman || !italic ? L"Cambria Math" : L"Cambria Math";
    if (FAILED(dwrite->CreateTextFormat(family, nullptr,
            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &fmt))) return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), 1000000.0f, 1000000.0f, &layout)))
        return nullptr;
    layout->GetMetrics(metrics);
    if (baseline) {
        DWRITE_LINE_METRICS lm;
        UINT32 count = 0;
        *baseline = size * 0.8f;
        if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &count)) && count > 0) *baseline = lm.baseline;
    }
    return layout;
}

float SpaceWidth(const MathNode& n, float fs) { return n.em * fs; }

} // namespace

MathBox MeasureMath(const MathNode& n, IDWriteFactory* dwrite, float fs) {
    MathBox box;
    switch (n.kind) {
    case MathKind::Text: {
        DWRITE_TEXT_METRICS tm{};
        FLOAT baseline = 0;
        auto layout = MakeTextLayout(dwrite, n.text, fs, n.italic, n.bold, n.roman, &tm, &baseline);
        if (!layout) { box.width = fs * 0.5f * (float)n.text.size(); box.ascent = fs * 0.8f; box.descent = fs * 0.2f; }
        else {
            box.width = tm.widthIncludingTrailingWhitespace;
            box.ascent = baseline;
            box.descent = std::max(1.0f, tm.height - baseline);
        }
        break;
    }
    case MathKind::Space:
        box.width = SpaceWidth(n, fs);
        box.ascent = 0;
        box.descent = 0;
        break;
    case MathKind::Row: {
        float width = 0, asc = 0, desc = 0;
        for (auto& it : n.items) {
            if (!it) continue;
            MathBox b = MeasureMath(*it, dwrite, fs);
            width += b.width;
            asc = std::max(asc, b.ascent);
            desc = std::max(desc, b.descent);
        }
        box.width = width; box.ascent = asc; box.descent = desc;
        break;
    }
    case MathKind::Fraction: {
        float fs2 = fs * 0.86f;
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs2) : MathBox{};
        MathBox b = n.b ? MeasureMath(*n.b, dwrite, fs2) : MathBox{};
        float axis = fs * 0.30f;
        float gap = fs * 0.14f;
        box.width = std::max(a.width, b.width) + fs * 0.30f;
        box.ascent = a.height() + gap + axis;
        box.descent = b.height() + gap - axis;
        break;
    }
    case MathKind::Sqrt: {
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs * 0.95f) : MathBox{};
        float rad = std::max(fs * 0.42f, a.height() * 0.38f);
        box.width = rad + a.width + fs * 0.12f;
        box.ascent = a.ascent + fs * 0.20f;
        box.descent = a.descent + fs * 0.05f;
        break;
    }
    case MathKind::Script: {
        float sf = fs * 0.68f;
        MathBox base = n.a ? MeasureMath(*n.a, dwrite, fs) : MathBox{};
        MathBox sup = n.b ? MeasureMath(*n.b, dwrite, sf) : MathBox{};
        MathBox sub = n.c ? MeasureMath(*n.c, dwrite, sf) : MathBox{};
        // A subscript (and to a lesser degree a superscript) has visible
        // overhang beyond the base advance. Give the whole script box a small
        // right side bearing so the next character does not touch it.
        float trailing = n.c ? fs * 0.14f : (n.b ? fs * 0.04f : 0.0f);
        if (n.displayLimits) {
            box.width = std::max({ base.width, sup.width, sub.width }) + trailing;
            box.ascent = base.ascent + (n.b ? sup.height() + fs * 0.12f : 0);
            box.descent = base.descent + (n.c ? sub.height() + fs * 0.10f : 0);
        } else {
            box.width = base.width + std::max(sup.width, sub.width) + fs * 0.08f + trailing;
            box.ascent = std::max(base.ascent, (n.b ? sup.height() : 0.0f) * 0.72f + base.ascent * 0.55f);
            box.descent = std::max(base.descent, (n.c ? sub.height() : 0.0f) * 0.72f + fs * 0.1f);
        }
        break;
    }
    case MathKind::Matrix: {
        float cellFs = fs * 0.92f;
        float gap = fs * 0.9f;
        float rowGap = fs * 0.3f;
        size_t cols = 0;
        for (auto& r : n.cells) cols = std::max(cols, r.size());
        std::vector<float> colW(cols, 0.0f);
        std::vector<float> rowH(n.cells.size(), 0.0f);
        std::vector<float> rowAsc(n.cells.size(), 0.0f);
        for (size_t r = 0; r < n.cells.size(); r++) {
            for (size_t c = 0; c < n.cells[r].size(); c++) {
                MathBox b = n.cells[r][c] ? MeasureMath(*n.cells[r][c], dwrite, cellFs) : MathBox{};
                colW[c] = std::max(colW[c], b.width);
                rowH[r] = std::max(rowH[r], b.height());
                rowAsc[r] = std::max(rowAsc[r], b.ascent);
            }
            rowH[r] = std::max(rowH[r], cellFs);
        }
        float width = 0;
        for (float w : colW) width += w + gap;
        if (!colW.empty()) width -= gap;
        float height = 0;
        for (float h : rowH) height += h + rowGap;
        if (!rowH.empty()) height -= rowGap;
        float del = (n.openDelim || n.closeDelim) ? std::max(fs * 0.5f, height * 0.28f) : 0;
        box.width = width + del * 2;
        box.ascent = height * 0.62f;
        box.descent = height * 0.38f;
        break;
    }
    case MathKind::Delim: {
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs) : MathBox{};
        float del = (n.openDelim || n.closeDelim) ? std::max(fs * 0.35f, a.height() * 0.28f) : 0;
        float closePad = n.closeDelim ? fs * 0.10f : 0.0f;
        box.width = a.width + del * 2 + closePad;
        box.ascent = std::max(a.ascent, a.height() * 0.62f);
        box.descent = std::max(a.descent, a.height() * 0.38f);
        break;
    }
    case MathKind::Accent: {
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs) : MathBox{};
        box.width = a.width + fs * 0.08f;
        box.ascent = a.ascent + fs * 0.28f;
        box.descent = a.descent + (n.accent == MathAccent::Underline ? fs * 0.18f : 0);
        break;
    }
    case MathKind::Line:
        box.width = fs;
        break;
    }
    return box;
}

namespace {

void DrawDelimiter(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, wchar_t ch,
                   float x, float top, float height, float fs) {
    if (!ch) return;
    float w = std::max(fs * 0.30f, height * 0.24f);
    float bottom = top + height;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory) return;
    factory->CreatePathGeometry(&path);
    path->Open(&sink);
    auto begin = [&](float px, float py) {
        sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_HOLLOW);
    };
    // Keep the original upper-lobe style (and width); only its direction was
    // wrong. The lower half is the same curve mirrored vertically.
    float braceW = w;
    // The caller anchors each glyph at its outer edge. Parenthesis tips sit
    // on that anchor and the curve bows toward the enclosed content, so the
    // left glyph opens right and the right glyph opens left.
    if (ch == L'(') {
        begin(x + w * 0.9f, top);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x, top + height * 0.15f),
                                           D2D1::Point2F(x, bottom - height * 0.15f),
                                           D2D1::Point2F(x + w * 0.9f, bottom)));
    } else if (ch == L')') {
        begin(x - w * 0.9f, top);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x, top + height * 0.15f),
                                           D2D1::Point2F(x, bottom - height * 0.15f),
                                           D2D1::Point2F(x - w * 0.9f, bottom)));
    } else if (ch == L'[') {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x + w * 0.75f, top));
        sink->AddLine(D2D1::Point2F(x, top));
        sink->AddLine(D2D1::Point2F(x, bottom));
        sink->AddLine(D2D1::Point2F(x + w * 0.75f, bottom));
    } else if (ch == L']') {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x - w * 0.75f, top));
        sink->AddLine(D2D1::Point2F(x, top));
        sink->AddLine(D2D1::Point2F(x, bottom));
        sink->AddLine(D2D1::Point2F(x - w * 0.75f, bottom));
    } else if (ch == L'{') {
        // Take the original (correct-looking) upper lobe, flip it horizontally
        // so its tip opens toward the content, and mirror that curve below.
        begin(x + braceW, top);
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(x, top),
            D2D1::Point2F(x + braceW * 0.85f, top + height * 0.25f),
            D2D1::Point2F(x + braceW * 0.25f, top + height * 0.5f)));
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(x + braceW * 0.85f, bottom - height * 0.25f),
            D2D1::Point2F(x, bottom),
            D2D1::Point2F(x + braceW, bottom)));
    } else if (ch == L'}') {
        begin(x - braceW, top);
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(x, top),
            D2D1::Point2F(x - braceW * 0.85f, top + height * 0.25f),
            D2D1::Point2F(x - braceW * 0.25f, top + height * 0.5f)));
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(x - braceW * 0.85f, bottom - height * 0.25f),
            D2D1::Point2F(x, bottom),
            D2D1::Point2F(x - braceW, bottom)));
    } else if (ch == L'⌊') {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x, bottom));
        sink->AddLine(D2D1::Point2F(x + w * 0.8f, bottom));
    } else if (ch == L'⌋') {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x, bottom));
        sink->AddLine(D2D1::Point2F(x - w * 0.8f, bottom));
    } else if (ch == L'⌈') {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x + w * 0.8f, top));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x, bottom));
    } else if (ch == L'⌉') {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x - w * 0.8f, top));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x, bottom));
    } else if (ch == L'|') {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x, bottom));
    } else if (ch == L'‖') {
        begin(x - w * 0.22f, top);
        sink->AddLine(D2D1::Point2F(x - w * 0.22f, bottom));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        begin(x + w * 0.22f, top);
        sink->AddLine(D2D1::Point2F(x + w * 0.22f, bottom));
    } else {
        begin(x, top);
        sink->AddLine(D2D1::Point2F(x, bottom));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    target->DrawGeometry(path.Get(), brush, std::max(1.0f, fs * 0.075f));
    factory->Release();
}

void DrawNode(const MathNode& n, ID2D1RenderTarget* target, IDWriteFactory* dwrite,
              ID2D1SolidColorBrush* brush, float x, float baseline, float fs) {
    switch (n.kind) {
    case MathKind::Text: {
        DWRITE_TEXT_METRICS tm{};
        FLOAT textBaseline = 0;
        auto layout = MakeTextLayout(dwrite, n.text, fs, n.italic, n.bold, n.roman, &tm, &textBaseline);
        if (layout) target->DrawTextLayout(D2D1::Point2F(x, baseline - textBaseline), layout.Get(), brush);
        break;
    }
    case MathKind::Space:
        break;
    case MathKind::Row: {
        float cx = x;
        for (auto& it : n.items) {
            if (!it) continue;
            MathBox b = MeasureMath(*it, dwrite, fs);
            DrawNode(*it, target, dwrite, brush, cx, baseline, fs);
            cx += b.width;
        }
        break;
    }
    case MathKind::Fraction: {
        float fs2 = fs * 0.86f;
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs2) : MathBox{};
        MathBox b = n.b ? MeasureMath(*n.b, dwrite, fs2) : MathBox{};
        float axis = fs * 0.30f;
        float gap = fs * 0.14f;
        float width = std::max(a.width, b.width) + fs * 0.30f;
        float numBase = baseline - axis - gap - a.descent;
        float denBase = baseline - axis + gap + b.ascent;
        if (n.a) DrawNode(*n.a, target, dwrite, brush, x + (width - a.width) / 2, numBase, fs2);
        if (n.b) DrawNode(*n.b, target, dwrite, brush, x + (width - b.width) / 2, denBase, fs2);
        target->DrawLine(D2D1::Point2F(x, baseline - axis), D2D1::Point2F(x + width, baseline - axis),
                         brush, std::max(1.0f, fs * 0.075f));
        break;
    }
    case MathKind::Sqrt: {
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs * 0.95f) : MathBox{};
        float rad = std::max(fs * 0.42f, a.height() * 0.38f);
        float top = baseline - a.ascent - fs * 0.20f;
        float bottom = baseline + a.descent;
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        ID2D1Factory* factory = nullptr;
        target->GetFactory(&factory);
        if (factory) {
            factory->CreatePathGeometry(&path);
            path->Open(&sink);
            sink->BeginFigure(D2D1::Point2F(x + 1, baseline + a.descent * 0.55f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(x + rad * 0.35f, bottom));
            sink->AddLine(D2D1::Point2F(x + rad * 0.72f, top + fs * 0.08f));
            sink->AddLine(D2D1::Point2F(x + rad + a.width + fs * 0.10f, top + fs * 0.08f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            target->DrawGeometry(path.Get(), brush, std::max(1.0f, fs * 0.075f));
            factory->Release();
        }
        if (n.a) DrawNode(*n.a, target, dwrite, brush, x + rad, baseline, fs * 0.95f);
        if (n.b) {
            DrawNode(*n.b, target, dwrite, brush, x + rad * 0.25f, top - fs * 0.08f, fs * 0.55f);
        }
        break;
    }
    case MathKind::Script: {
        MathBox base = n.a ? MeasureMath(*n.a, dwrite, fs) : MathBox{};
        float sf = fs * 0.68f;
        MathBox sup = n.b ? MeasureMath(*n.b, dwrite, sf) : MathBox{};
        MathBox sub = n.c ? MeasureMath(*n.c, dwrite, sf) : MathBox{};
        if (n.displayLimits) {
            float width = std::max({ base.width, sup.width, sub.width });
            if (n.a) DrawNode(*n.a, target, dwrite, brush, x + (width - base.width) / 2, baseline, fs);
            if (n.b) DrawNode(*n.b, target, dwrite, brush, x + (width - sup.width) / 2,
                              baseline - base.ascent - fs * 0.12f - sup.descent, sf);
            if (n.c) DrawNode(*n.c, target, dwrite, brush, x + (width - sub.width) / 2,
                              baseline + base.descent + fs * 0.10f + sub.ascent, sf);
        } else {
            if (n.a) DrawNode(*n.a, target, dwrite, brush, x, baseline, fs);
            float rx = x + base.width + fs * 0.06f;
            if (n.b) DrawNode(*n.b, target, dwrite, brush, rx, baseline - base.ascent * 0.55f - fs * 0.04f, sf);
            if (n.c) DrawNode(*n.c, target, dwrite, brush, rx, baseline + base.descent * 0.45f + fs * 0.12f, sf);
        }
        break;
    }
    case MathKind::Matrix: {
        float cellFs = fs * 0.92f;
        float gap = fs * 0.9f;
        float rowGap = fs * 0.3f;
        size_t cols = 0;
        for (auto& r : n.cells) cols = std::max(cols, r.size());
        std::vector<float> colW(cols, 0.0f);
        std::vector<float> rowH(n.cells.size(), 0.0f);
        std::vector<float> rowAsc(n.cells.size(), 0.0f);
        float width = 0, height = 0;
        for (size_t r = 0; r < n.cells.size(); r++) {
            for (size_t c = 0; c < n.cells[r].size(); c++) {
                MathBox b = n.cells[r][c] ? MeasureMath(*n.cells[r][c], dwrite, cellFs) : MathBox{};
                colW[c] = std::max(colW[c], b.width);
                rowH[r] = std::max(rowH[r], b.height());
                rowAsc[r] = std::max(rowAsc[r], b.ascent);
            }
            rowH[r] = std::max(rowH[r], cellFs);
        }
        for (float w : colW) width += w + gap;
        if (!colW.empty()) width -= gap;
        for (float h : rowH) height += h + rowGap;
        if (!rowH.empty()) height -= rowGap;
        float del = (n.openDelim || n.closeDelim) ? std::max(fs * 0.5f, height * 0.28f) : 0;
        float left = x + del;
        float top = baseline - height * 0.62f;
        float cy = top;
        for (size_t r = 0; r < n.cells.size(); r++) {
            float cx = left;
            float cellBaseline = cy + rowAsc[r];
            for (size_t c = 0; c < n.cells[r].size(); c++) {
                MathBox b = n.cells[r][c] ? MeasureMath(*n.cells[r][c], dwrite, cellFs) : MathBox{};
                if (n.cells[r][c]) DrawNode(*n.cells[r][c], target, dwrite, brush,
                                            cx + (colW[c] - b.width) / 2, cellBaseline, cellFs);
                cx += colW[c] + gap;
            }
            cy += rowH[r] + rowGap;
        }
        if (n.openDelim) DrawDelimiter(target, brush, n.openDelim, x + del * 0.45f, top, height, fs);
        if (n.closeDelim) DrawDelimiter(target, brush, n.closeDelim, x + width + del * 1.55f, top, height, fs);
        break;
    }
    case MathKind::Delim: {
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs) : MathBox{};
        float del = (n.openDelim || n.closeDelim) ? std::max(fs * 0.35f, a.height() * 0.28f) : 0;
        float closePad = n.closeDelim ? fs * 0.10f : 0.0f;
        float top = baseline - std::max(a.ascent, a.height() * 0.62f);
        float height = std::max(a.height(), fs);
        if (n.openDelim) DrawDelimiter(target, brush, n.openDelim, x + del * 0.45f, top, height, fs);
        if (n.a) DrawNode(*n.a, target, dwrite, brush, x + del, baseline, fs);
        if (n.closeDelim)
            DrawDelimiter(target, brush, n.closeDelim, x + del + a.width + closePad + del * 0.55f, top, height, fs);
        break;
    }
    case MathKind::Accent: {
        MathBox a = n.a ? MeasureMath(*n.a, dwrite, fs) : MathBox{};
        if (n.a) DrawNode(*n.a, target, dwrite, brush, x + fs * 0.04f, baseline, fs);
        float cx = x + fs * 0.04f + a.width / 2;
        float top = baseline - a.ascent;
        if (n.accent == MathAccent::Overline) {
            target->DrawLine(D2D1::Point2F(cx - a.width / 2, top - fs * 0.10f),
                             D2D1::Point2F(cx + a.width / 2, top - fs * 0.10f), brush, std::max(1.0f, fs * 0.06f));
        } else if (n.accent == MathAccent::Underline) {
            target->DrawLine(D2D1::Point2F(cx - a.width / 2, baseline + a.descent + fs * 0.10f),
                             D2D1::Point2F(cx + a.width / 2, baseline + a.descent + fs * 0.10f), brush, std::max(1.0f, fs * 0.06f));
        } else if (n.accent == MathAccent::Vec) {
            float y = top - fs * 0.18f;
            target->DrawLine(D2D1::Point2F(cx - a.width / 2, y), D2D1::Point2F(cx + a.width / 2, y), brush, 1.2f);
            target->DrawLine(D2D1::Point2F(cx + a.width / 2, y),
                             D2D1::Point2F(cx + a.width / 2 - fs * 0.12f, y - fs * 0.08f), brush, 1.2f);
        } else if (n.accent == MathAccent::Hat) {
            float y = top - fs * 0.12f;
            target->DrawLine(D2D1::Point2F(cx - a.width * 0.3f, y), D2D1::Point2F(cx, y - fs * 0.15f), brush, 1.1f);
            target->DrawLine(D2D1::Point2F(cx, y - fs * 0.15f), D2D1::Point2F(cx + a.width * 0.3f, y), brush, 1.1f);
        } else if (n.accent == MathAccent::Dot || n.accent == MathAccent::Ddot) {
            float y = top - fs * 0.12f;
            float r = std::max(1.0f, fs * 0.045f);
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y), r, r), brush);
            if (n.accent == MathAccent::Ddot)
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + fs * 0.16f, y), r, r), brush);
        } else {
            // tilde: draw a small wave with two arcs approximated by lines
            float y = top - fs * 0.10f;
            target->DrawLine(D2D1::Point2F(cx - a.width * 0.3f, y), D2D1::Point2F(cx - a.width * 0.1f, y - fs * 0.07f), brush, 1.0f);
            target->DrawLine(D2D1::Point2F(cx - a.width * 0.1f, y - fs * 0.07f), D2D1::Point2F(cx + a.width * 0.1f, y), brush, 1.0f);
            target->DrawLine(D2D1::Point2F(cx + a.width * 0.1f, y), D2D1::Point2F(cx + a.width * 0.3f, y - fs * 0.07f), brush, 1.0f);
        }
        break;
    }
    case MathKind::Line:
        break;
    }
}

} // namespace

std::shared_ptr<MathNode> ParseMath(const std::wstring& latex, bool display) {
    if (Trim(latex).empty()) return nullptr;
    MathParser p(latex, display);
    auto node = p.ParseRow();
    return node;
}

void DrawMath(const MathNode& node, ID2D1RenderTarget* target, IDWriteFactory* dwrite,
              float x, float baseline, float fontSize, const Color& color) {
    if (!target || !dwrite) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(D2D1::ColorF(color.r, color.g, color.b, color.a), &brush);
    if (!brush) return;
    DrawNode(node, target, dwrite, brush.Get(), x, baseline, fontSize);
}

std::wstring MathPlainText(const MathNode& n) {
    switch (n.kind) {
    case MathKind::Text: return n.text;
    case MathKind::Space: return L" ";
    case MathKind::Row: {
        std::wstring out;
        for (auto& i : n.items) if (i) out += MathPlainText(*i);
        return out;
    }
    case MathKind::Fraction:
        return (n.a ? MathPlainText(*n.a) : L"") + L"/" + (n.b ? MathPlainText(*n.b) : L"");
    case MathKind::Sqrt:
        return L"√(" + (n.a ? MathPlainText(*n.a) : L"") + L")";
    case MathKind::Script: {
        std::wstring out = n.a ? MathPlainText(*n.a) : L"";
        if (n.b) out += L"^" + MathPlainText(*n.b);
        if (n.c) out += L"_" + MathPlainText(*n.c);
        return out;
    }
    case MathKind::Matrix: {
        std::wstring out;
        for (auto& row : n.cells) {
            if (!out.empty()) out += L"; ";
            for (size_t c = 0; c < row.size(); c++) {
                if (c) out += L", ";
                if (row[c]) out += MathPlainText(*row[c]);
            }
        }
        return out;
    }
    case MathKind::Delim:
        return std::wstring(1, n.openDelim ? n.openDelim : L' ') + (n.a ? MathPlainText(*n.a) : L"") +
               std::wstring(1, n.closeDelim ? n.closeDelim : L' ');
    case MathKind::Accent:
        return n.a ? MathPlainText(*n.a) : L"";
    case MathKind::Line:
        return L"";
    }
    return L"";
}
