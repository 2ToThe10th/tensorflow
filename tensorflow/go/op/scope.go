/*
Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package op

import (
	"fmt"
	"runtime/debug"

	tf "github.com/tensorflow/tensorflow/tensorflow/go"
)

// Scope encapsulates common operation properties when building a Graph.
//
// A Scope object (and its derivates, e.g., obtained from Scope.SubScope)
// act as a builder for graphs. They allow common properties (such as
// a name prefix) to be specified for multiple operations being added
// to the graph.
//
// A Scope object and all its derivates (e.g., obtained from Scope.SubScope)
// are not safe for concurrent use by multiple goroutines.
type Scope struct {
	graph     *tf.Graph
	namemap   map[string]int
	namespace string
	err       *scopeErr
}

// scopeErr is used to share errors between all derivatives of a root scope.
type scopeErr struct {
	err error
}

// NewScope creates a Scope initialized with an empty Graph.
func NewScope() *Scope {
	return &Scope{graph: tf.NewGraph(), namemap: make(map[string]int), err: new(scopeErr)}
}

// Finalize returns the Graph on which this scope operates on and renders s
// unusable. If there was an error during graph construction, that error is
// returned instead.
func (s *Scope) Finalize() (*tf.Graph, error) {
	if err := s.Err(); err != nil {
		return nil, err
	}
	s.err.err = fmt.Errorf("Scope has been finalized and is no longer usable")
	return s.graph, nil
}

// AddOperation adds the operation to the Graph managed by s.
//
// If there is a name prefix associated with s (such as if s was created
// by a call to SubScope), then this prefix will be applied to the name
// of the operation being added. See also Graph.AddOperation.
func (s *Scope) AddOperation(args tf.OpSpec) *tf.Operation {
	if s.Err() != nil {
		return nil
	}
	if args.Name == "" {
		args.Name = args.Type
	}
	if s.namespace != "" {
		args.Name = s.namespace + "/" + args.Name
	}
	op, err := s.graph.AddOperation(args)
	if err != nil {
		s.UpdateErr(args.Type, err)
	}
	return op
}

// SubScope returns a new Scope which will cause all operations added to the
// graph to be namespaced with 'namespace'.  If namespace collides with an
// existing namespace within the scope, then a suffix will be added.
func (s *Scope) SubScope(namespace string) *Scope {
	namespace = s.uniqueName(namespace)
	if s.namespace != "" {
		namespace = s.namespace + "/" + namespace
	}
	return &Scope{
		graph:     s.graph,
		namemap:   make(map[string]int),
		namespace: namespace,
		err:       s.err,
	}
}

// Err returns the error, if any, encountered during the construction
// of the Graph managed by s.
//
// Once Err returns a non-nil error, all future calls will do the same,
// indicating that the scope should be discarded as the graph could not
// be constructed.
func (s *Scope) Err() error {
	return s.err.err
}

// UpdateErr is used to notify Scope of any graph construction errors
// while creating the operation op.
func (s *Scope) UpdateErr(op string, err error) {
	if s.err.err == nil {
		s.err.err = fmt.Errorf("failed to add operation %q: %v (Stacktrace: %s)", op, err, debug.Stack())
	}
}

func (s *Scope) uniqueName(name string) string {
	count := s.namemap[name]
	s.namemap[name]++
	if count == 0 {
		return name
	}
	return fmt.Sprint(name, "_", count)
}

func (s *Scope) opName(typ string) string {
	if s.namespace == "" {
		return typ
	}
	return s.namespace + "/" + typ
}
