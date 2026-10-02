package config

import (
	"bytes"
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// Promote rewrites the config file at path so that store becomes the primary
// and the old primary becomes a mirror. Comments and other keys are kept.
func Promote(path, store string) error {
	raw, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var doc yaml.Node
	if err := yaml.Unmarshal(raw, &doc); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	stores := mappingValue(doc.Content[0], "stores")
	if stores == nil || mappingValue(stores, store) == nil {
		return fmt.Errorf("%s: no store named %q", path, store)
	}
	for i := 0; i < len(stores.Content); i += 2 {
		name, body := stores.Content[i].Value, stores.Content[i+1]
		role := mappingValue(body, "role")
		if role == nil {
			continue
		}
		switch {
		case name == store:
			role.Value = string(RolePrimary)
		case role.Value == string(RolePrimary):
			role.Value = string(RoleMirror)
		}
	}
	var out bytes.Buffer
	enc := yaml.NewEncoder(&out)
	enc.SetIndent(2)
	if err := enc.Encode(&doc); err != nil {
		return err
	}
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, out.Bytes(), info.Mode().Perm()); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func mappingValue(n *yaml.Node, key string) *yaml.Node {
	if n == nil || n.Kind != yaml.MappingNode {
		return nil
	}
	for i := 0; i+1 < len(n.Content); i += 2 {
		if n.Content[i].Value == key {
			return n.Content[i+1]
		}
	}
	return nil
}
