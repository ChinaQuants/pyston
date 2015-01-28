// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/dict.h"

#include "capi/types.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

Box* dictRepr(BoxedDict* self) {
    std::vector<char> chars;
    chars.push_back('{');
    bool first = true;
    for (const auto& p : self->d) {
        if (!first) {
            chars.push_back(',');
            chars.push_back(' ');
        }
        first = false;

        BoxedString* k = static_cast<BoxedString*>(repr(p.first));
        BoxedString* v = static_cast<BoxedString*>(repr(p.second));
        chars.insert(chars.end(), k->s.begin(), k->s.end());
        chars.push_back(':');
        chars.push_back(' ');
        chars.insert(chars.end(), v->s.begin(), v->s.end());
    }
    chars.push_back('}');
    return boxString(std::string(chars.begin(), chars.end()));
}

Box* dictClear(BoxedDict* self) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor 'clear' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    self->d.clear();
    return None;
}

Box* dictCopy(BoxedDict* self) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor 'copy' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    BoxedDict* r = new BoxedDict();
    r->d.insert(self->d.begin(), self->d.end());
    return r;
}

Box* dictItems(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();

    for (const auto& p : self->d) {
        BoxedTuple::GCVector elts;
        elts.push_back(p.first);
        elts.push_back(p.second);
        BoxedTuple* t = new BoxedTuple(std::move(elts));
        listAppendInternal(rtn, t);
    }

    return rtn;
}

Box* dictValues(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    for (const auto& p : self->d) {
        listAppendInternal(rtn, p.second);
    }
    return rtn;
}

Box* dictKeys(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    for (const auto& p : self->d) {
        listAppendInternal(rtn, p.first);
    }
    return rtn;
}

extern "C" PyObject* PyDict_Keys(PyObject* mp) noexcept {
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    try {
        return dictKeys(static_cast<BoxedDict*>(mp));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

Box* dictViewKeys(BoxedDict* self) {
    if (!isSubclass(self->cls, dict_cls)) {
        raiseExcHelper(TypeError, "descriptor 'viewkeys' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());
    }
    BoxedDictView* rtn = new (dict_keys_cls) BoxedDictView(self);
    return rtn;
}

Box* dictViewValues(BoxedDict* self) {
    if (!isSubclass(self->cls, dict_cls)) {
        raiseExcHelper(TypeError, "descriptor 'viewvalues' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());
    }
    BoxedDictView* rtn = new (dict_values_cls) BoxedDictView(self);
    return rtn;
}

Box* dictViewItems(BoxedDict* self) {
    if (!isSubclass(self->cls, dict_cls)) {
        raiseExcHelper(TypeError, "descriptor 'viewitems' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());
    }
    BoxedDictView* rtn = new (dict_items_cls) BoxedDictView(self);
    return rtn;
}

Box* dictLen(BoxedDict* self) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor '__len__' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    return boxInt(self->d.size());
}

extern "C" Py_ssize_t PyDict_Size(PyObject* op) noexcept {
    RELEASE_ASSERT(PyDict_Check(op), "");
    return static_cast<BoxedDict*>(op)->d.size();
}

extern "C" void PyDict_Clear(PyObject* op) noexcept {
    RELEASE_ASSERT(PyDict_Check(op), "");
    static_cast<BoxedDict*>(op)->d.clear();
}

extern "C" PyObject* PyDict_Copy(PyObject* o) noexcept {
    RELEASE_ASSERT(PyDict_Check(o), "");
    try {
        return dictCopy(static_cast<BoxedDict*>(o));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyDict_Update(PyObject* a, PyObject* b) noexcept {
    return PyDict_Merge(a, b, 1);
}

Box* dictGetitem(BoxedDict* self, Box* k) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor '__getitem__' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        BoxedString* s = reprOrNull(k);

        if (s)
            raiseExcHelper(KeyError, "%s", s->s.c_str());
        else
            raiseExcHelper(KeyError, "");
    }

    Box* pos = self->d[k];

    return pos;
}

extern "C" PyObject* PyDict_New() noexcept {
    return new BoxedDict();
}

// We don't assume that dicts passed to PyDict are necessarily dicts, since there are a couple places
// that we provide dict-like objects instead of proper dicts.
// The performance should hopefully be comparable to the CPython fast case, since we can use
// runtimeICs.
extern "C" int PyDict_SetItem(PyObject* mp, PyObject* _key, PyObject* _item) noexcept {
    ASSERT(mp->cls == dict_cls || mp->cls == attrwrapper_cls, "%s", getTypeName(mp)->c_str());

    assert(mp);
    Box* b = static_cast<Box*>(mp);
    Box* key = static_cast<Box*>(_key);
    Box* item = static_cast<Box*>(_item);

    try {
        // TODO should demote GIL?
        setitem(b, key, item);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

extern "C" int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item) noexcept {
    Box* key_s;
    try {
        key_s = boxStrConstant(key);
    } catch (ExcInfo e) {
        abort();
    }

    return PyDict_SetItem(mp, key_s, item);
}

extern "C" PyObject* PyDict_GetItem(PyObject* dict, PyObject* key) noexcept {
    ASSERT(dict->cls == dict_cls || dict->cls == attrwrapper_cls, "%s", getTypeName(dict)->c_str());
    try {
        return getitem(dict, key);
    } catch (ExcInfo e) {
        if (e.matches(KeyError))
            return NULL;
        abort();
    }
}

extern "C" int PyDict_Next(PyObject* op, Py_ssize_t* ppos, PyObject** pkey, PyObject** pvalue) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyDict_GetItemString(PyObject* dict, const char* key) noexcept {
    Box* key_s;
    try {
        key_s = boxStrConstant(key);
    } catch (ExcInfo e) {
        abort();
    }
    return PyDict_GetItem(dict, key_s);
}

Box* dictSetitem(BoxedDict* self, Box* k, Box* v) {
    // printf("Starting setitem\n");
    Box*& pos = self->d[k];
    // printf("Got the pos\n");

    if (pos != NULL) {
        pos = v;
    } else {
        pos = v;
    }

    return None;
}

Box* dictDelitem(BoxedDict* self, Box* k) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor '__delitem__' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        BoxedString* s = reprOrNull(k);

        if (s)
            raiseExcHelper(KeyError, "%s", s->s.c_str());
        else
            raiseExcHelper(KeyError, "");
    }

    self->d.erase(it);

    return None;
}

extern "C" int PyDict_DelItem(PyObject* op, PyObject* key) noexcept {
    try {
        dictDelitem((BoxedDict*)op, key);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }

    return 0;
}
Box* dictPop(BoxedDict* self, Box* k, Box* d) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor 'pop' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        if (d)
            return d;

        BoxedString* s = reprOrNull(k);

        if (s)
            raiseExcHelper(KeyError, "%s", s->s.c_str());
        else
            raiseExcHelper(KeyError, "");
    }

    Box* rtn = it->second;
    self->d.erase(it);
    return rtn;
}

Box* dictPopitem(BoxedDict* self) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor 'popitem' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    auto it = self->d.begin();
    if (it == self->d.end()) {
        raiseExcHelper(KeyError, "popitem(): dictionary is empty");
    }

    Box* key = it->first;
    Box* value = it->second;
    self->d.erase(it);

    auto rtn = new BoxedTuple({ key, value });
    return rtn;
}

Box* dictGet(BoxedDict* self, Box* k, Box* d) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor 'get' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    auto it = self->d.find(k);
    if (it == self->d.end())
        return d;

    return it->second;
}

Box* dictSetdefault(BoxedDict* self, Box* k, Box* v) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor 'setdefault' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    auto it = self->d.find(k);
    if (it != self->d.end())
        return it->second;

    self->d.insert(it, std::make_pair(k, v));
    return v;
}

Box* dictContains(BoxedDict* self, Box* k) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor '__contains__' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    return boxBool(self->d.count(k) != 0);
}

Box* dictNonzero(BoxedDict* self) {
    return boxBool(self->d.size());
}

Box* dictFromkeys(BoxedDict* self, Box* iterable, Box* default_value) {
    if (!isSubclass(self->cls, dict_cls))
        raiseExcHelper(TypeError, "descriptor 'fromkeys' requires a 'dict' object but received a '%s'",
                       getTypeName(self)->c_str());

    auto rtn = new BoxedDict();
    for (Box* e : iterable->pyElements()) {
        dictSetitem(rtn, e, default_value);
    }

    return rtn;
}



extern "C" Box* dictNew(Box* _cls, BoxedTuple* args, BoxedDict* kwargs) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "dict.__new__(X): X is not a type object (%s)", getTypeName(_cls)->c_str());

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, dict_cls))
        raiseExcHelper(TypeError, "dict.__new__(%s): %s is not a subtype of dict", getNameOfClass(cls)->c_str(),
                       getNameOfClass(cls)->c_str());

    return new (cls) BoxedDict();
}

void dictMerge(BoxedDict* self, Box* other) {
    if (other->cls == dict_cls) {
        for (const auto& p : static_cast<BoxedDict*>(other)->d)
            self->d[p.first] = p.second;
        return;
    }

    static const std::string keys_str("keys");
    Box* keys = callattr(other, &keys_str, CallattrFlags({.cls_only = false, .null_on_nonexistent = true }),
                         ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    assert(keys);

    for (Box* k : keys->pyElements()) {
        self->d[k] = getitem(other, k);
    }
}

void dictMergeFromSeq2(BoxedDict* self, Box* other) {
    int idx = 0;

    // raises if not iterable
    for (const auto& element : other->pyElements()) {

        // should this check subclasses? anyway to check if something is iterable...
        if (element->cls == list_cls) {
            BoxedList* list = static_cast<BoxedList*>(element);
            if (list->size != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %d; 2 is required", idx,
                               list->size);

            self->d[list->elts->elts[0]] = list->elts->elts[1];
        } else if (element->cls == tuple_cls) {
            BoxedTuple* tuple = static_cast<BoxedTuple*>(element);
            if (tuple->elts.size() != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %d; 2 is required", idx,
                               tuple->elts.size());

            self->d[tuple->elts[0]] = tuple->elts[1];
        } else
            raiseExcHelper(TypeError, "cannot convert dictionary update sequence element #%d to a sequence", idx);

        idx++;
    }
}

extern "C" int PyDict_Merge(PyObject* a, PyObject* b, int override_) noexcept {
    if (a == NULL || !PyDict_Check(a) || b == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }

    if (override_ != 1)
        Py_FatalError("unimplemented");

    try {
        dictMerge(static_cast<BoxedDict*>(a), b);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* dictUpdate(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(kwargs);
    assert(kwargs->cls == dict_cls);

    RELEASE_ASSERT(args->elts.size() <= 1, ""); // should throw a TypeError
    if (args->elts.size()) {
        Box* arg = args->elts[0];
        if (getattrInternal(arg, "keys", NULL)) {
            dictMerge(self, arg);
        } else {
            dictMergeFromSeq2(self, arg);
        }
    }

    if (kwargs->d.size())
        dictMerge(self, kwargs);

    return None;
}

extern "C" Box* dictInit(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    int args_sz = args->elts.size();
    int kwargs_sz = kwargs->d.size();

    // CPython accepts a single positional and keyword arguments, in any combination
    if (args_sz > 1)
        raiseExcHelper(TypeError, "dict expected at most 1 arguments, got %d", args_sz);

    dictUpdate(self, args, kwargs);

    // handle keyword arguments by merging (possibly over positional entries per CPy)
    assert(kwargs->cls == dict_cls);

    for (const auto& p : kwargs->d)
        self->d[p.first] = p.second;

    return None;
}

extern "C" int PyMapping_Check(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyMapping_Size(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyMapping_HasKeyString(PyObject* o, char* key) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyMapping_HasKey(PyObject* o, PyObject* key) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyMapping_GetItemString(PyObject* o, char* key) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyMapping_SetItemString(PyObject* o, char* key, PyObject* v) noexcept {
    Py_FatalError("unimplemented");
}

BoxedClass* dict_iterator_cls = NULL;
extern "C" void dictIteratorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedDictIterator* it = static_cast<BoxedDictIterator*>(b);
    v->visit(it->d);
}

BoxedClass* dict_keys_cls = NULL;
BoxedClass* dict_values_cls = NULL;
BoxedClass* dict_items_cls = NULL;
extern "C" void dictViewGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedDictView* view = static_cast<BoxedDictView*>(b);
    v->visit(view->d);
}

void setupDict() {
    dict_iterator_cls = new BoxedHeapClass(object_cls, &dictIteratorGCHandler, 0, sizeof(BoxedDict), false);

    dict_keys_cls = new BoxedHeapClass(object_cls, &dictViewGCHandler, 0, sizeof(BoxedDictView), false);
    dict_values_cls = new BoxedHeapClass(object_cls, &dictViewGCHandler, 0, sizeof(BoxedDictView), false);
    dict_items_cls = new BoxedHeapClass(object_cls, &dictViewGCHandler, 0, sizeof(BoxedDictView), false);

    dict_cls->giveAttr("__name__", boxStrConstant("dict"));
    dict_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)dictLen, BOXED_INT, 1)));
    dict_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)dictNew, UNKNOWN, 1, 0, true, true)));
    dict_cls->giveAttr("__init__", new BoxedFunction(boxRTFunction((void*)dictInit, NONE, 1, 0, true, true)));
    dict_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)dictRepr, STR, 1)));

    dict_cls->giveAttr("__iter__",
                       new BoxedFunction(boxRTFunction((void*)dictIterKeys, typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("update", new BoxedFunction(boxRTFunction((void*)dictUpdate, NONE, 1, 0, true, true)));

    dict_cls->giveAttr("clear", new BoxedFunction(boxRTFunction((void*)dictClear, NONE, 1)));
    dict_cls->giveAttr("copy", new BoxedFunction(boxRTFunction((void*)dictCopy, DICT, 1)));

    dict_cls->giveAttr("has_key", new BoxedFunction(boxRTFunction((void*)dictContains, BOXED_BOOL, 2)));
    dict_cls->giveAttr("fromkeys",
                       new BoxedFunction(boxRTFunction((void*)dictFromkeys, DICT, 3, 1, false, false), { None }));
    dict_cls->giveAttr("items", new BoxedFunction(boxRTFunction((void*)dictItems, LIST, 1)));
    dict_cls->giveAttr("iteritems",
                       new BoxedFunction(boxRTFunction((void*)dictIterItems, typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("values", new BoxedFunction(boxRTFunction((void*)dictValues, LIST, 1)));
    dict_cls->giveAttr("itervalues",
                       new BoxedFunction(boxRTFunction((void*)dictIterValues, typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("keys", new BoxedFunction(boxRTFunction((void*)dictKeys, LIST, 1)));
    dict_cls->giveAttr("iterkeys", dict_cls->getattr("__iter__"));

    dict_cls->giveAttr("pop", new BoxedFunction(boxRTFunction((void*)dictPop, UNKNOWN, 3, 1, false, false), { NULL }));
    dict_cls->giveAttr("popitem", new BoxedFunction(boxRTFunction((void*)dictPopitem, BOXED_TUPLE, 1)));


    dict_cls->giveAttr("viewkeys", new BoxedFunction(boxRTFunction((void*)dictViewKeys, UNKNOWN, 1)));
    dict_cls->giveAttr("viewvalues", new BoxedFunction(boxRTFunction((void*)dictViewValues, UNKNOWN, 1)));
    dict_cls->giveAttr("viewitems", new BoxedFunction(boxRTFunction((void*)dictViewItems, UNKNOWN, 1)));

    dict_cls->giveAttr("get", new BoxedFunction(boxRTFunction((void*)dictGet, UNKNOWN, 3, 1, false, false), { None }));

    dict_cls->giveAttr("setdefault",
                       new BoxedFunction(boxRTFunction((void*)dictSetdefault, UNKNOWN, 3, 1, false, false), { None }));

    dict_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)dictGetitem, UNKNOWN, 2)));
    dict_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)dictSetitem, NONE, 3)));
    dict_cls->giveAttr("__delitem__", new BoxedFunction(boxRTFunction((void*)dictDelitem, UNKNOWN, 2)));
    dict_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)dictContains, BOXED_BOOL, 2)));

    dict_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)dictNonzero, BOXED_BOOL, 1)));

    dict_cls->freeze();

    dict_iterator_cls->giveAttr("__name__", boxStrConstant("dictiterator"));

    CLFunction* hasnext = boxRTFunction((void*)dictIterHasnextUnboxed, BOOL, 1);
    addRTFunction(hasnext, (void*)dictIterHasnext, BOXED_BOOL);
    dict_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    dict_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)dictIterIter, typeFromClass(dict_iterator_cls), 1)));
    dict_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)dictIterNext, UNKNOWN, 1)));

    dict_iterator_cls->freeze();

    dict_keys_cls->giveAttr("__name__", boxStrConstant("dictkeys"));
    dict_keys_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)dictViewKeysIter, typeFromClass(dict_iterator_cls), 1)));
    dict_keys_cls->freeze();
    dict_values_cls->giveAttr("__name__", boxStrConstant("dictvalues"));
    dict_values_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)dictViewValuesIter, typeFromClass(dict_iterator_cls), 1)));
    dict_values_cls->freeze();
    dict_items_cls->giveAttr("__name__", boxStrConstant("dictitems"));
    dict_items_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)dictViewItemsIter, typeFromClass(dict_iterator_cls), 1)));
    dict_items_cls->freeze();
}

void teardownDict() {
}
}
