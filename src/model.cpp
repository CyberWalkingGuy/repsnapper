/*
    This file is a part of the RepSnapper project.
    Copyright (C) 2011 Michael Meeks

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "config.h"
#define  MODEL_IMPLEMENTATION
#include <vector>
#include <string>
#include <cerrno>
#include <functional>

#include <glib/gutils.h>
#include <libreprap/comms.h>

#include "stdafx.h"
#include "model.h"
#include "objtree.h"
#include "file.h"
#include "settings.h"
#include "connectview.h"

Model::Model() :
  settings(),
  Min(), Max(),
  errlog (Gtk::TextBuffer::create()),
  echolog (Gtk::TextBuffer::create())
{
  // Variable defaults
  Center.x = Center.y = 100.0;
  Center.z = 0.0;
}

Model::~Model()
{
  ClearCuttingPlanes();
  ClearGCode();
}

void Model::alert (const char *message)
{
  signal_alert.emit (Gtk::MESSAGE_INFO, message, NULL);
}

void Model::error (const char *message, const char *secondary)
{
  signal_alert.emit (Gtk::MESSAGE_ERROR, message, secondary);
}

void Model::SaveConfig(Glib::RefPtr<Gio::File> file)
{
  settings.save_settings(file);
}

void Model::LoadConfig(Glib::RefPtr<Gio::File> file)
{
  settings.load_settings(file);
  ModelChanged();
}

void Model::SimpleAdvancedToggle()
{
  cout << _("not yet implemented\n");
}

void Model::ReadGCode(Glib::RefPtr<Gio::File> file)
{
  m_progress.start (_("Converting"), 100.0);
  gcode.Read (this, &m_progress, file->get_path());
  m_progress.stop (_("Done"));
  ModelChanged();
}

void Model::ClearGCode()
{
  gcode.clear();
}

void Model::ClearCuttingPlanes()
{
  for(uint i=0;i<cuttingplanes.size();i++)
    cuttingplanes[i]->Clear();
  cuttingplanes.clear();
}

Glib::RefPtr<Gtk::TextBuffer> Model::GetGCodeBuffer()
{
  return gcode.buffer;
}

void Model::GlDrawGCode()
{
  gcode.draw (settings);
}

void Model::init() {}

void Model::WriteGCode(Glib::RefPtr<Gio::File> file)
{
  Glib::ustring contents = gcode.get_text();
  Glib::file_set_contents (file->get_path(), contents);
}


void Model::ReadStl(Glib::RefPtr<Gio::File> file)
{
  Shape stl;
  if (stl.load (file->get_path()) == 0)
    AddStl(NULL, stl, file->get_path());
  ModelChanged();
  ClearCuttingPlanes();
}

void Model::Read(Glib::RefPtr<Gio::File> file)
{
  std::string basename = file->get_basename();
  size_t pos = basename.rfind('.');
  if (pos != std::string::npos) {
    std::string extn = basename.substr(pos);
    if (extn == ".gcode")
      {
	ReadGCode (file);
	return;
      }
    else if (extn == ".rfo")
      {
	//      ReadRFO (file);
	return;
      }
  }
  ReadStl (file);
}

void Model::ModelChanged()
{
  //printer.update_temp_poll_interval(); // necessary?
  CalcBoundingBoxAndCenter();
  m_model_changed.emit();
}

static bool ClosestToOrigin (Vector3d a, Vector3d b)
{
  return (a.x*a.x + a.y*a.y + a.z*a.z) < (b.x*b.x + b.y*b.y + b.z*b.z);
}


bool Model::FindEmptyLocation(Vector3d &result, Shape *shape)
{
  // Get all object positions
  vector<Vector3d> maxpos;
  vector<Vector3d> minpos;

  for(uint o=0;o<objtree.Objects.size();o++)
  {
    for(uint f=0;f<objtree.Objects[o].shapes.size();f++)
    {
      Shape *selectedShape = &objtree.Objects[o].shapes[f];
      Vector3d p = selectedShape->transform3D.transform.getTranslation();
      Vector3d size = selectedShape->Max - selectedShape->Min;

      minpos.push_back(Vector3d(p.x, p.y, p.z));
      maxpos.push_back(Vector3d(p.x+size.x, p.y+size.y, p.z));
    }
  }

  // Get all possible object positions

  double d = 5.0; // 5mm spacing between objects
  Vector3d StlDelta = (shape->Max - shape->Min);
  vector<Vector3d> candidates;

  candidates.push_back(Vector3d(0.0, 0.0, 0.0));

  for (uint j=0; j<maxpos.size(); j++)
  {
    candidates.push_back(Vector3d(maxpos[j].x+d, minpos[j].y, maxpos[j].z));
    candidates.push_back(Vector3d(minpos[j].x, maxpos[j].y+d, maxpos[j].z));
    candidates.push_back(Vector3d(maxpos[j].x+d, maxpos[j].y+d, maxpos[j].z));
  }

  // Prefer positions closest to origin
  sort(candidates.begin(), candidates.end(), ClosestToOrigin);

  // Check all candidates for collisions with existing objects
  for (uint c=0; c<candidates.size(); c++)
  {
    bool ok = true;

    for (uint k=0; k<maxpos.size(); k++)
    {
      if (
          // check x
          ((minpos[k].x <= candidates[c].x && candidates[c].x <= maxpos[k].x) ||
          (candidates[c].x <= minpos[k].x && maxpos[k].x <= candidates[c].x+StlDelta.x+d) ||
          (minpos[k].x <= candidates[c].x+StlDelta.x+d && candidates[c].x+StlDelta.x+d <= maxpos[k].x))
          &&
          // check y
          ((minpos[k].y <= candidates[c].y && candidates[c].y <= maxpos[k].y) ||
          (candidates[c].y <= minpos[k].y && maxpos[k].y <= candidates[c].y+StlDelta.y+d) ||
          (minpos[k].y <= candidates[c].y+StlDelta.y+d && candidates[c].y+StlDelta.y+d <= maxpos[k].y))
      )
      {
        ok = false;
        break;
      }

      // volume boundary
      if (candidates[c].x+StlDelta.x > (settings.Hardware.Volume.x - 2*settings.Hardware.PrintMargin.x) ||
          candidates[c].y+StlDelta.y > (settings.Hardware.Volume.y - 2*settings.Hardware.PrintMargin.y))
      {
        ok = false;
        break;
      }
    }
    if (ok)
    {
      result.x = candidates[c].x;
      result.y = candidates[c].y;
      result.z = candidates[c].z;
      return true;
    }
  }

  // no empty spots
  return false;
}

Shape* Model::AddStl(TreeObject *parent, Shape shape, string filename)
{
  Shape *retshape;
  bool found_location;

  if (!parent) {
    if (objtree.Objects.size() <= 0)
      objtree.newObject();
    parent = &objtree.Objects.back();
  }
  g_assert (parent != NULL);

  // Decide where it's going
  Vector3d trans = Vector3d(0,0,0);
  found_location = FindEmptyLocation(trans, &shape);

  // Add it to the set
  size_t found = filename.find_last_of("/\\");
  Gtk::TreePath path = objtree.addShape(parent, shape, filename.substr(found+1));
  retshape = &parent->shapes.back();

  // Move it, if we found a suitable place
  if (found_location)
    retshape->transform3D.transform.setTranslation(trans);

  // Update the view to include the new object
  CalcBoundingBoxAndCenter();

  // Tell everyone
  m_signal_stl_added.emit (path);

  return retshape;
}

void Model::newObject()
{
  objtree.newObject();
}

/* Scales the object on changes of the scale slider */
void Model::ScaleObject(Shape *shape, TreeObject *object, double scale)
{
  if (!shape)
    return;

  shape->Scale(scale);
  CalcBoundingBoxAndCenter();
}

void Model::RotateObject(Shape *shape, TreeObject *object, Vector4d rotate)
{
  Vector3d rot(rotate.x, rotate.y, rotate.z);

  if (!shape)
    return; // FIXME: rotate entire Objects ...

  shape->Rotate(rot, rotate.w);
  CalcBoundingBoxAndCenter();
}

void Model::OptimizeRotation(Shape *shape, TreeObject *object)
{
  if (!shape)
    return; // FIXME: rotate entire Objects ...

  shape->OptimizeRotation();
  CalcBoundingBoxAndCenter();
}

void Model::DeleteObjTree(Gtk::TreeModel::iterator &iter)
{
  objtree.DeleteSelected (iter);
  ClearGCode();
  ClearCuttingPlanes();
  CalcBoundingBoxAndCenter();
}


void Model::ClearLogs()
{
  errlog->set_text("");
  echolog->set_text("");
}

void Model::CalcBoundingBoxAndCenter()
{
  Vector3d newMax = Vector3d(G_MINDOUBLE, G_MINDOUBLE, G_MINDOUBLE);
  Vector3d newMin = Vector3d(G_MAXDOUBLE, G_MAXDOUBLE, G_MAXDOUBLE);

  for (uint i = 0 ; i < objtree.Objects.size(); i++) {
    for (uint j = 0; j < objtree.Objects[i].shapes.size(); j++) {
      Matrix4d M = objtree.GetSTLTransformationMatrix (i, j);
      Vector3d stlMin = M * objtree.Objects[i].shapes[j].Min;
      Vector3d stlMax = M * objtree.Objects[i].shapes[j].Max;
      for (uint k = 0; k < 3; k++) {
	newMin.xyz[k] = MIN(stlMin.xyz[k], newMin.xyz[k]);
	newMax.xyz[k] = MAX(stlMax.xyz[k], newMax.xyz[k]);
      }
    }
  }

  if (newMin.x > newMax.x) {
    // Show the whole platform if there's no objects
    Min = Vector3d(0,0,0);
    Vector3d pM = settings.Hardware.PrintMargin;
    Max = settings.Hardware.Volume - pM - pM;
    Max.z = 0;
  }
  else {
    Max = newMax;
    Min = newMin;
  }

  Center = (Max + Min) / 2.0;
  m_signal_tree_changed.emit();
}

Vector3d Model::GetViewCenter()
{
  Vector3d printOffset = settings.Hardware.PrintMargin;
  if(settings.RaftEnable)
    printOffset += Vector3d(settings.Raft.Size, settings.Raft.Size, 0);

  return printOffset + Center;
}

// called from View::Draw
void Model::draw (Gtk::TreeModel::iterator &iter)
{

  Shape *sel_shape;
  TreeObject *sel_object;
  gint index = 1; // pick/select index. matches computation in update_model()
  objtree.get_selected_stl (iter, sel_object, sel_shape);

  Vector3d printOffset = settings.Hardware.PrintMargin;
  if(settings.RaftEnable)
    printOffset += Vector3d(settings.Raft.Size, settings.Raft.Size, 0);
  Vector3d translation = objtree.transform3D.transform.getTranslation();
  Vector3d offset = printOffset + translation;
  
  // Add the print offset to the drawing location of the STL objects.
  glTranslatef(offset.x, offset.y, offset.z);

  glPushMatrix();
  glMultMatrixd (&objtree.transform3D.transform.array[0]);

  for (uint i = 0; i < objtree.Objects.size(); i++) {
    TreeObject *object = &objtree.Objects[i];
    index++;

    glPushMatrix();
    glMultMatrixd (&object->transform3D.transform.array[0]);
    for (uint j = 0; j < object->shapes.size(); j++) {
      Shape *shape = &object->shapes[j];
      glLoadName(index); // Load select/pick index
      index++;
      glPushMatrix();
      glMultMatrixd (&shape->transform3D.transform.array[0]);

      bool is_selected = (sel_shape == shape ||
	  (!sel_shape && sel_object == object));

      if (is_selected) {
        // Enable stencil buffer when we draw the selected object.
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 1);
        glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);

        shape->draw (this, settings);

        if (!settings.Display.DisplayPolygons) {
                // If not drawing polygons, need to draw the geometry
                // manually, but invisible, to set up the stencil buffer
                glEnable(GL_CULL_FACE);
                glEnable(GL_DEPTH_TEST);
                glEnable(GL_BLEND);
                // Set to not draw anything, and not update depth buffer
                glDepthMask(GL_FALSE);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

                shape->draw_geometry();

                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
        }

        // draw highlight around selected object
        glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
        glLineWidth(3.0);
	glEnable (GL_POLYGON_OFFSET_LINE);

        glDisable (GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilFunc(GL_NOTEQUAL, 1, 1);
	glEnable(GL_DEPTH_TEST);

	shape->draw_geometry();

        glEnable (GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_STENCIL_TEST);
	glDisable(GL_POLYGON_OFFSET_LINE);
      }
      else {
        shape->draw (this, settings);
      }

      glPopMatrix();
    }
    glPopMatrix();
  }
  glPopMatrix();
  glLoadName(0); // Clear selection name to avoid selecting last object with later rendering.

  // draw total bounding box
  if(settings.Display.DisplayBBox)
    {
      // Draw bbox
      glDisable(GL_DEPTH_TEST);
      glColor3f(1,0,0);
      glBegin(GL_LINE_LOOP);
      glVertex3f(Min.x, Min.y, Min.z);
      glVertex3f(Min.x, Max.y, Min.z);
      glVertex3f(Max.x, Max.y, Min.z);
      glVertex3f(Max.x, Min.y, Min.z);
      glEnd();
      glBegin(GL_LINE_LOOP);
      glVertex3f(Min.x, Min.y, Max.z);
      glVertex3f(Min.x, Max.y, Max.z);
      glVertex3f(Max.x, Max.y, Max.z);
      glVertex3f(Max.x, Min.y, Max.z);
      glEnd();
      glBegin(GL_LINES);
      glVertex3f(Min.x, Min.y, Min.z);
      glVertex3f(Min.x, Min.y, Max.z);
      glVertex3f(Min.x, Max.y, Min.z);
      glVertex3f(Min.x, Max.y, Max.z);
      glVertex3f(Max.x, Max.y, Min.z);
      glVertex3f(Max.x, Max.y, Max.z);
      glVertex3f(Max.x, Min.y, Min.z);
      glVertex3f(Max.x, Min.y, Max.z);
      glEnd();
    }

  if(settings.Display.DisplayCuttingPlane) {
    glDisable(GL_DEPTH_TEST);
    drawCuttingPlanes(offset);
  }
  
}

void Model::drawCuttingPlanes(Vector3d offset) const
{
  uint LayerNr;

  bool have_planes = cuttingplanes.size() > 0; // have sliced already

  double z;
  double zStep = settings.Hardware.LayerThickness;
  double zSize = (Max.z-Min.z);
  uint LayerCount = (uint)ceil((zSize + zStep*0.5)/zStep);
  double sel_Z = settings.Display.CuttingPlaneValue*zSize;
  uint sel_Layer;
  if (have_planes) 
      sel_Layer = (uint)ceil(settings.Display.CuttingPlaneValue*(cuttingplanes.size()-1));
  else 
      sel_Layer = (uint)ceil(LayerCount*(sel_Z)/zSize);
  LayerCount = sel_Layer+1;
  if(settings.Display.DisplayAllLayers)
    {
      LayerNr = 0;
      z=Min.z + 0.5*zStep;
    } else 
    {
      LayerNr = sel_Layer;
      z=Min.z + sel_Z;
    }
  //cerr << zStep << ";"<<Max.z<<";"<<Min.z<<";"<<zSize<<";"<<LayerNr<<";"<<LayerCount<<";"<<endl;

  vector<int> altInfillLayers;
  settings.Slicing.GetAltInfillLayers (altInfillLayers, LayerCount);
  
  CuttingPlane* plane;
  if (have_planes) 
    glTranslatef(-offset.x, -offset.y, -offset.z);

  double matwidth;
  
  while(LayerNr < LayerCount)
    {
      if (have_planes)
	{
	  plane = cuttingplanes[LayerNr];
	  z = plane->getZ();
	}
      else
	{
	  plane = new CuttingPlane(LayerNr,settings.Hardware.LayerThickness);
	  plane->setZ(z);
	  matwidth = settings.Hardware.GetExtrudedMaterialWidth(plane->thickness);
	  for(size_t o=0;o<objtree.Objects.size();o++)
	    {
	      for(size_t f=0;f<objtree.Objects[o].shapes.size();f++)
		{
		  Matrix4d T = objtree.GetSTLTransformationMatrix(o,f);
		  Vector3d t = T.getTranslation();
		  T.setTranslation(t);
		  objtree.Objects[o].shapes[f].CalcCuttingPlane(T, settings.Slicing.Optimization, plane);
		}
	    }

	  plane->MakePolygons(settings.Slicing.Optimization);

	  bool makeskirt = (plane->getZ() <= settings.Slicing.SkirtHeight);
	  uint skins = settings.Slicing.Skins;

	  plane->MakeShells(//settings.Slicing.ShrinkQuality,
			    settings.Slicing.ShellCount,
			    matwidth, settings.Slicing.Optimization,
			    makeskirt, skins, false);
	  if (settings.Display.DisplayinFill)
	    {
	      double fullInfillDistance = matwidth;
	      double infillDistance = fullInfillDistance *(1+settings.Slicing.InfillDistance);
	      double altInfillDistance = fullInfillDistance *(1+settings.Slicing.AltInfillDistance);
	      double infilldist;
	      if (std::find(altInfillLayers.begin(), altInfillLayers.end(), LayerNr) 
		  != altInfillLayers.end())
		infilldist = altInfillDistance;
	      else
		infilldist = infillDistance;
	      plane->CalcInfill(infillDistance, fullInfillDistance,
				settings.Slicing.InfillRotation,
				settings.Slicing.InfillRotationPrLayer, 
				settings.Slicing.ShellOnly,
				settings.Display.DisplayDebuginFill);
	    }
	}
      plane->Draw(settings.Display.DrawVertexNumbers,
		  settings.Display.DrawLineNumbers,
		  settings.Display.DrawCPOutlineNumbers,
		  settings.Display.DrawCPLineNumbers, 
		  settings.Display.DrawCPVertexNumbers,
		  settings.Display.DisplayinFill);

      if (!have_planes)
      {
            // need to delete the temporary cutting plane
            delete plane;
      }
      // displayInfillOld(settings, plane, plane.LayerNo, altInfillLayers);
      
      LayerNr++; 
      z+=zStep;
    }// while
}

