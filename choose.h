#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/vector.h>

namespace stl {
    class ObjPool;
}

struct Ui;

enum class ChooseMode : u8 {
    Open,
    Save,
    Directory
};

struct ChooseOptions {
    ChooseMode mode = ChooseMode::Open;
    bool multiple = false;
    stl::StringView title;
    stl::StringView name;
    stl::StringView start;
    stl::Vector<stl::StringView> filters;
};

struct VisitChosen {
    virtual void visit(stl::StringView path) = 0;
};

template <typename F>
struct ChosenVisitor: public VisitChosen {
    F fn;

    ChosenVisitor(F f)
        : fn(f)
    {
    }

    void visit(stl::StringView path) override {
        fn(path);
    }
};

struct Chooser {
    virtual bool draw(VisitChosen& chosen) = 0;

    static Chooser* create(stl::ObjPool& pool, Ui& ui, const ChooseOptions& options);
};

bool chooseInWindow(Ui& ui, const ChooseOptions& options, VisitChosen& chosen);

template <typename F>
bool chooseInWindow(Ui& ui, const ChooseOptions& options, F f) {
    ChosenVisitor<F> v(f);

    return chooseInWindow(ui, options, (VisitChosen&)v);
}

int mainChoose(stl::ObjPool& pool, int argc, char** argv);
